//====================================================
//          Http Lib for lua scripting
//
// should add flag HAVE_CURL and have curl installed
// to be able to use this
//
// this will allow addons to have access to http
// features
//
// this is intended for server-side addons
// but could be use anywhere
//
// made by flaffy for ZE ReBlood Server :p
//====================================================

#include "lua_httplib.h"
#include "doomdef.h"
#include "lua_script.h"
#include "lua_libs.h"
#include "z_zone.h"
#include "i_threads.h"
#include "console.h"

#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

typedef struct {
	char *data;
	size_t size;
} http_buffer_t;

typedef struct {
	char *key;
	char *value;
} http_header_t;

typedef struct http_response_s {
	long status;
	char *body;
	http_header_t *headers;
	size_t header_count;
	char *final_url;
} http_response_t;

typedef struct {
	char *url;
	char *method;
	char *body;
	http_header_t *headers;
	size_t header_count;
	int timeout;
	boolean follow_redirects;
	long max_redirects;
	int callback_ref;
	lua_State *L;
} http_request_t;

typedef struct pending_callback_s {
	int callback_ref;
	http_response_t *response;
	char *error_msg;
	lua_State *L;
	struct pending_callback_s *next;
} pending_callback_t;

typedef struct {
	pending_callback_t *head;
	pending_callback_t *tail;
	I_mutex mutex;
} callback_queue_t;

static char *default_user_agent = NULL;
static int default_timeout = 10;
static http_header_t *default_headers = NULL;
static size_t default_header_count = 0;
static callback_queue_t callback_queue = {NULL, NULL, NULL};

static void enqueue_callback(lua_State *L, int callback_ref, http_response_t *response, char *error_msg)
{
	pending_callback_t *cb = malloc(sizeof(pending_callback_t));

	if (!cb)
	{
		I_Error("Out of memory");
		return;
	}

	cb->callback_ref = callback_ref;
	cb->response = response;
	cb->error_msg = error_msg;
	cb->L = L;
	cb->next = NULL;
	
	I_lock_mutex(&callback_queue.mutex);
	
	if (callback_queue.tail == NULL) {
		callback_queue.head = cb;
		callback_queue.tail = cb;
	} else {
		callback_queue.tail->next = cb;
		callback_queue.tail = cb;
	}
	
	I_unlock_mutex(callback_queue.mutex);
}

static pending_callback_t* dequeue_callback(void)
{
	I_lock_mutex(&callback_queue.mutex);
	
	pending_callback_t *cb = callback_queue.head;
	if (cb != NULL) {
		callback_queue.head = cb->next;
		if (callback_queue.head == NULL)
			callback_queue.tail = NULL;
	}
	
	I_unlock_mutex(callback_queue.mutex);
	return cb;
}

#ifdef HAVE_CURL
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	http_buffer_t *mem = (http_buffer_t *)userp;
	
	char *ptr = realloc(mem->data, mem->size + realsize + 1);
	if (!ptr)
		return 0;
	
	mem->data = ptr;
	memcpy(&(mem->data[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->data[mem->size] = 0;
	
	return realsize;
}

static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
	size_t numbytes = size * nitems;
	http_response_t *resp = (http_response_t *)userdata;
	char *colon = strchr(buffer, ':');
	
	if (colon && numbytes > 2) {
		size_t key_len = colon - buffer;
		size_t val_start = key_len + 1;
		
		while (val_start < numbytes && (buffer[val_start] == ' ' || buffer[val_start] == '\t'))
			val_start++;
		
		size_t val_len = numbytes - val_start;
		while (val_len > 0 && (buffer[val_start + val_len - 1] == '\r' ||
			buffer[val_start + val_len - 1] == '\n'))
		{
			val_len--;
		}
		
		if (val_len > 0) {
			http_header_t *tmp = realloc(resp->headers, sizeof(http_header_t) * (resp->header_count + 1));

			if (!tmp)
			{
				free(resp->headers);
				I_Error("Out of memory");
				return 0;
			}
			else
				resp->headers = tmp;

			resp->headers[resp->header_count].key = malloc(key_len + 1);
			if (!resp->headers[resp->header_count].key)
				return 0;

			resp->headers[resp->header_count].value = malloc(val_len + 1);
			if (!resp->headers[resp->header_count].value)
				return 0;
			
			memcpy(resp->headers[resp->header_count].key, buffer, key_len);
			resp->headers[resp->header_count].key[key_len] = 0;
			
			memcpy(resp->headers[resp->header_count].value, buffer + val_start, val_len);
			resp->headers[resp->header_count].value[val_len] = 0;
			
			resp->header_count++;
		}
	}
	
	return numbytes;
}

static http_response_t* perform_http_request(http_request_t *req, char **error_msg)
{
	CURL *curl;
	CURLcode res;
	http_buffer_t chunk = {0};
	http_response_t *response = calloc(1, sizeof(http_response_t));
	struct curl_slist *headers = NULL;
	size_t i;
	
	curl = curl_easy_init();
	if (!curl) {
		*error_msg = strdup("Failed to initialize CURL");
		free(response);
		return NULL;
	}
	
	curl_easy_setopt(curl, CURLOPT_URL, req->url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, req->timeout);
	
	if (default_user_agent)
		curl_easy_setopt(curl, CURLOPT_USERAGENT, default_user_agent);
	else
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "SRB2-Banpyura/" VERSIONSTRING);
	
	if (req->follow_redirects) {
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, req->max_redirects);
	}
	
	for (i = 0; i < default_header_count; i++) {
		char *header_str = malloc(strlen(default_headers[i].key) + strlen(default_headers[i].value) + 3);
		sprintf(header_str, "%s: %s", default_headers[i].key, default_headers[i].value);
		headers = curl_slist_append(headers, header_str);
		free(header_str);
	}
	
	for (i = 0; i < req->header_count; i++) {
		char *header_str = malloc(strlen(req->headers[i].key) + strlen(req->headers[i].value) + 3);
		sprintf(header_str, "%s: %s", req->headers[i].key, req->headers[i].value);
		headers = curl_slist_append(headers, header_str);
		free(header_str);
	}
	
	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	
	if (strcmp(req->method, "POST") == 0) {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		if (req->body)
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
	} else if (strcmp(req->method, "PUT") == 0) {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
		if (req->body)
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
	} else if (strcmp(req->method, "DELETE") == 0) {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
	} else if (strcmp(req->method, "PATCH") == 0) {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
		if (req->body)
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
	}
	
	res = curl_easy_perform(curl);
	
	if (res != CURLE_OK) {
		*error_msg = strdup(curl_easy_strerror(res));
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		free(chunk.data);
		free(response);
		return NULL;
	}
	
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
	
	char *final_url;
	curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final_url);
	response->final_url = strdup(final_url);
	
	response->body = chunk.data;
	
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	
	return response;
}

static void free_response(http_response_t *resp)
{
	size_t i;
	if (!resp) return;
	
	free(resp->body);
	free(resp->final_url);
	
	for (i = 0; i < resp->header_count; i++) {
		free(resp->headers[i].key);
		free(resp->headers[i].value);
	}
	free(resp->headers);
	free(resp);
}

static void free_request(http_request_t *req)
{
	size_t i;
	if (!req) return;
	
	free(req->url);
	free(req->method);
	free(req->body);
	
	for (i = 0; i < req->header_count; i++) {
		free(req->headers[i].key);
		free(req->headers[i].value);
	}
	free(req->headers);
	free(req);
}

static void push_response_table(lua_State *L, http_response_t *resp)
{
	size_t i;
	lua_newtable(L);
	
	lua_pushinteger(L, resp->status);
	lua_setfield(L, -2, "status");
	
	lua_pushstring(L, resp->body);
	lua_setfield(L, -2, "body");
	
	lua_pushstring(L, resp->final_url);
	lua_setfield(L, -2, "url");
	
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "success");
	
	lua_newtable(L);
	for (i = 0; i < resp->header_count; i++) {
		lua_pushstring(L, resp->headers[i].value);
		lua_setfield(L, -2, resp->headers[i].key);
	}
	lua_setfield(L, -2, "headers");
}

static void push_error_table(lua_State *L, const char *error_msg)
{
	lua_newtable(L);
	
	lua_pushstring(L, error_msg);
	lua_setfield(L, -2, "message");
	
	lua_pushstring(L, "CURL_ERROR");
	lua_setfield(L, -2, "type");
}

static http_request_t* parse_request_options(lua_State *L, int index)
{
	http_request_t *req = calloc(1, sizeof(http_request_t));
	
	lua_getfield(L, index, "url");
	req->url = strdup(luaL_checkstring(L, -1));
	lua_pop(L, 1);
	
	lua_getfield(L, index, "method");
	if (lua_isstring(L, -1))
		req->method = strdup(lua_tostring(L, -1));
	else
		req->method = strdup("GET");
	lua_pop(L, 1);
	
	lua_getfield(L, index, "body");
	if (lua_isstring(L, -1))
		req->body = strdup(lua_tostring(L, -1));
	lua_pop(L, 1);
	
	lua_getfield(L, index, "timeout");
	req->timeout = lua_isnumber(L, -1) ? lua_tointeger(L, -1) : default_timeout;
	lua_pop(L, 1);
	
	lua_getfield(L, index, "followRedirects");
	req->follow_redirects = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : true;
	lua_pop(L, 1);
	
	lua_getfield(L, index, "maxRedirects");
	req->max_redirects = lua_isnumber(L, -1) ? lua_tointeger(L, -1) : 50;
	lua_pop(L, 1);
	
	lua_getfield(L, index, "headers");
	if (lua_istable(L, -1)) {
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
				http_header_t *tmp = realloc(req->headers, sizeof(http_header_t) * (req->header_count + 1));

				if (!tmp)
				{
					free(req->headers);
					I_Error("Out of memory");
					return NULL;
				}
				else
					req->headers = tmp;

				req->headers[req->header_count].key = strdup(lua_tostring(L, -2));
				req->headers[req->header_count].value = strdup(lua_tostring(L, -1));
				req->header_count++;
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
	
	return req;
}

static void http_worker_thread(void *arg)
{
	http_request_t *req = (http_request_t *)arg;
	char *error_msg = NULL;
	http_response_t *resp = perform_http_request(req, &error_msg);
	
	enqueue_callback(req->L, req->callback_ref, resp, error_msg);
	free_request(req);
}
#endif

static int lib_http_request(lua_State *L)
{
#ifndef HAVE_CURL
	return luaL_error(L, "This build lacks curl support");
#else
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	
	http_request_t *req = parse_request_options(L, 1);
	req->L = L;
	lua_pushvalue(L, 2);
	req->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	if (!I_spawn_thread("http-worker", http_worker_thread, req)) {
		luaL_unref(L, LUA_REGISTRYINDEX, req->callback_ref);
		free_request(req);
		return luaL_error(L, "Failed to spawn HTTP worker thread");
	}
	
	return 0;
#endif
}

static int lib_http_get(lua_State *L)
{
#ifndef HAVE_CURL
	return luaL_error(L, "This build lacks curl support");
#else
	const char *url = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	
	http_request_t *req = calloc(1, sizeof(http_request_t));

	if (!req)
	{
		I_Error("Out of memory");
		return 0;
	}

	req->url = strdup(url);
	req->method = strdup("GET");
	req->timeout = default_timeout;
	req->follow_redirects = true;
	req->max_redirects = 50;
	req->L = L;
	
	if (lua_istable(L, 3)) {
		lua_pushnil(L);
		while (lua_next(L, 3) != 0) {
			if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
				http_header_t *tmp = realloc(req->headers, sizeof(http_header_t) * (req->header_count + 1));

				if (!tmp)
				{
					free(req->headers);
					I_Error("Out of memory");
					return 0;
				}
				else
					req->headers = tmp;

				req->headers[req->header_count].key = strdup(lua_tostring(L, -2));
				req->headers[req->header_count].value = strdup(lua_tostring(L, -1));
				req->header_count++;
			}
			lua_pop(L, 1);
		}
	}
	
	lua_pushvalue(L, 2);
	req->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	
	if (!I_spawn_thread("http-worker", http_worker_thread, req)) {
		luaL_unref(L, LUA_REGISTRYINDEX, req->callback_ref);
		free_request(req);
		return luaL_error(L, "Failed to spawn HTTP worker thread");
	}
	
	return 0;
#endif
}

static int lib_http_post(lua_State *L)
{
#ifndef HAVE_CURL
	return luaL_error(L, "This build lacks curl support");
#else
	const char *url = luaL_checkstring(L, 1);
	const char *body = luaL_checkstring(L, 2);
	luaL_checktype(L, 3, LUA_TFUNCTION);
	
	http_request_t *req = calloc(1, sizeof(http_request_t));

	if (!req)
	{
		I_Error("Out of memory");
		return 0;
	}

	req->url = strdup(url);
	req->method = strdup("POST");
	req->body = strdup(body);
	req->timeout = default_timeout;
	req->follow_redirects = true;
	req->max_redirects = 50;
	req->L = L;

	if (lua_istable(L, 4)) {
		lua_pushnil(L);
		while (lua_next(L, 4) != 0) {
			if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
				http_header_t *tmp = realloc(req->headers, sizeof(http_header_t) * (req->header_count + 1));

				if (!tmp)
				{
					free(req->headers);
					I_Error("Out of memory");
					return 0;
				}
				else
					req->headers = tmp;

				req->headers[req->header_count].key = strdup(lua_tostring(L, -2));
				req->headers[req->header_count].value = strdup(lua_tostring(L, -1));
				req->header_count++;
			}
			lua_pop(L, 1);
		}
	}
	
	lua_pushvalue(L, 3);
	req->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	
	if (!I_spawn_thread("http-worker", http_worker_thread, req)) {
		luaL_unref(L, LUA_REGISTRYINDEX, req->callback_ref);
		free_request(req);
		return luaL_error(L, "Failed to spawn HTTP worker thread");
	}
	
	return 0;
#endif
}

static int lib_http_put(lua_State *L)
{
#ifndef HAVE_CURL
	return luaL_error(L, "This build lacks curl support");
#else
	const char *url = luaL_checkstring(L, 1);
	const char *body = luaL_checkstring(L, 2);
	luaL_checktype(L, 3, LUA_TFUNCTION);
	
	http_request_t *req = calloc(1, sizeof(http_request_t));

	if (!req)
	{
		I_Error("Out of memory");
		return 0;
	}

	req->url = strdup(url);
	req->method = strdup("PUT");
	req->body = strdup(body);
	req->timeout = default_timeout;
	req->follow_redirects = true;
	req->max_redirects = 50;
	req->L = L;
	
	if (lua_istable(L, 4)) {
		lua_pushnil(L);
		while (lua_next(L, 4) != 0) {
			if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
				http_header_t *tmp = realloc(req->headers, sizeof(http_header_t) * (req->header_count + 1));

				if (!tmp)
				{
					free(req->headers);
					I_Error("Out of memory");
					return 0;
				}
				else
					req->headers = tmp;

				req->headers[req->header_count].key = strdup(lua_tostring(L, -2));
				req->headers[req->header_count].value = strdup(lua_tostring(L, -1));
				req->header_count++;
			}
			lua_pop(L, 1);
		}
	}

	lua_pushvalue(L, 3);
	req->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	if (!I_spawn_thread("http-worker", http_worker_thread, req)) {
		luaL_unref(L, LUA_REGISTRYINDEX, req->callback_ref);
		free_request(req);
		return luaL_error(L, "Failed to spawn HTTP worker thread");
	}
	
	return 0;
#endif
}

static int lib_http_delete(lua_State *L)
{
#ifndef HAVE_CURL
	return luaL_error(L, "This build lacks curl support");
#else
	const char *url = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	
	http_request_t *req = calloc(1, sizeof(http_request_t));

	if (!req)
	{
		I_Error("Out of memory");
		return 0;
	}

	req->url = strdup(url);
	req->method = strdup("DELETE");
	req->timeout = default_timeout;
	req->follow_redirects = true;
	req->max_redirects = 50;
	req->L = L;
	
	if (lua_istable(L, 3)) {
		lua_pushnil(L);
		while (lua_next(L, 3) != 0) {
			if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
				http_header_t *tmp = realloc(req->headers, sizeof(http_header_t) * (req->header_count + 1));

				if (!tmp)
				{
					free(req->headers);
					I_Error("Out of memory");
					return 0;
				}
				else
					req->headers = tmp;

				req->headers[req->header_count].key = strdup(lua_tostring(L, -2));
				req->headers[req->header_count].value = strdup(lua_tostring(L, -1));
				req->header_count++;
			}
			lua_pop(L, 1);
		}
	}
	
	lua_pushvalue(L, 2);
	req->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	
	if (!I_spawn_thread("http-worker", http_worker_thread, req)) {
		luaL_unref(L, LUA_REGISTRYINDEX, req->callback_ref);
		free_request(req);
		return luaL_error(L, "Failed to spawn HTTP worker thread");
	}
	
	return 0;
#endif
}

static int lib_http_patch(lua_State *L)
{
#ifndef HAVE_CURL
	return luaL_error(L, "This build lacks curl support");
#else
	const char *url = luaL_checkstring(L, 1);
	const char *body = luaL_checkstring(L, 2);
	luaL_checktype(L, 3, LUA_TFUNCTION);
	
	http_request_t *req = calloc(1, sizeof(http_request_t));

	if (!req)
	{
		I_Error("Out of memory");
		return 0;
	}

	req->url = strdup(url);
	req->method = strdup("PATCH");
	req->body = strdup(body);
	req->timeout = default_timeout;
	req->follow_redirects = true;
	req->max_redirects = 50;
	req->L = L;
	
	if (lua_istable(L, 4)) {
		lua_pushnil(L);
		while (lua_next(L, 4) != 0) {
			if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
				http_header_t *tmp = realloc(req->headers, sizeof(http_header_t) * (req->header_count + 1));

				if (!tmp)
				{
					free(req->headers);
					I_Error("Out of memory");
					return 0;
				}
				else
					req->headers = tmp;

				req->headers[req->header_count].key = strdup(lua_tostring(L, -2));
				req->headers[req->header_count].value = strdup(lua_tostring(L, -1));
				req->header_count++;
			}
			lua_pop(L, 1);
		}
	}
	
	lua_pushvalue(L, 3);
	req->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	if (!I_spawn_thread("http-worker", http_worker_thread, req)) {
		luaL_unref(L, LUA_REGISTRYINDEX, req->callback_ref);
		free_request(req);
		return luaL_error(L, "Failed to spawn HTTP worker thread");
	}
	
	return 0;
#endif
}

static int lib_http_encode_url(lua_State *L)
{
	const char *str = luaL_checkstring(L, 1);
	
#ifdef HAVE_CURL
	CURL *curl = curl_easy_init();
	if (curl) {
		char *encoded = curl_easy_escape(curl, str, 0);
		lua_pushstring(L, encoded);
		curl_free(encoded);
		curl_easy_cleanup(curl);
		return 1;
	}
#endif
	
	lua_pushstring(L, str);
	return 1;
}

static int lib_http_decode_url(lua_State *L)
{
	const char *str = luaL_checkstring(L, 1);
	
#ifdef HAVE_CURL
	CURL *curl = curl_easy_init();
	if (curl) {
		int outlength;
		char *decoded = curl_easy_unescape(curl, str, 0, &outlength);
		lua_pushstring(L, decoded);
		curl_free(decoded);
		curl_easy_cleanup(curl);
		return 1;
	}
#endif
	
	lua_pushstring(L, str);
	return 1;
}

static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int lib_http_encode_base64(lua_State *L)
{
	size_t len, i, j;
	const unsigned char *data = (const unsigned char *)luaL_checklstring(L, 1, &len);
	size_t out_len = 4 * ((len + 2) / 3);
	char *encoded = malloc(out_len + 1);

	if (!encoded)
	{
		I_Error("Out of memory");
		return 0;
	}
	
	for (i = 0, j = 0; i < len;) {
		uint32_t octet_a = i < len ? data[i++] : 0;
		uint32_t octet_b = i < len ? data[i++] : 0;
		uint32_t octet_c = i < len ? data[i++] : 0;
		uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
		
		encoded[j++] = base64_chars[(triple >> 3 * 6) & 0x3F];
		encoded[j++] = base64_chars[(triple >> 2 * 6) & 0x3F];
		encoded[j++] = base64_chars[(triple >> 1 * 6) & 0x3F];
		encoded[j++] = base64_chars[(triple >> 0 * 6) & 0x3F];
	}
	
	for (i = 0; i < (3 - len % 3) % 3; i++)
		encoded[out_len - 1 - i] = '=';
	
	encoded[out_len] = '\0';
	lua_pushstring(L, encoded);
	free(encoded);
	
	return 1;
}

static int lib_http_decode_base64(lua_State *L)
{
	size_t len, i, j;
	const char *data = luaL_checklstring(L, 1, &len);
	size_t out_len = len / 4 * 3;
	
	if (len > 0 && data[len - 1] == '=') out_len--;
	if (len > 1 && data[len - 2] == '=') out_len--;
	
	unsigned char *decoded = malloc(out_len + 1);

	if (!decoded)
	{
		I_Error("Out of memory");
		return 0;
	}
	
	for (i = 0, j = 0; i < len;) {
		size_t sextet_a = data[i] == '=' ? 0 & i++ : (size_t)(strchr(base64_chars, data[i++]) - base64_chars);
		size_t sextet_b = data[i] == '=' ? 0 & i++ : (size_t)(strchr(base64_chars, data[i++]) - base64_chars);
		size_t sextet_c = data[i] == '=' ? 0 & i++ : (size_t)(strchr(base64_chars, data[i++]) - base64_chars);
		size_t sextet_d = data[i] == '=' ? 0 & i++ : (size_t)(strchr(base64_chars, data[i++]) - base64_chars);
		size_t triple = (sextet_a << 3 * 6) + (sextet_b << 2 * 6) + (sextet_c << 1 * 6) + (sextet_d << 0 * 6);
		
		if (j < out_len) decoded[j++] = (triple >> 2 * 8) & 0xFF;
		if (j < out_len) decoded[j++] = (triple >> 1 * 8) & 0xFF;
		if (j < out_len) decoded[j++] = (triple >> 0 * 8) & 0xFF;
	}
	
	decoded[out_len] = '\0';
	lua_pushlstring(L, (const char *)decoded, out_len);
	free(decoded);
	
	return 1;
}

static int lib_http_build_query(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_Buffer buf;
	luaL_buffinit(L, &buf);
	int first = 1;
	
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		if (lua_isstring(L, -2) && (lua_isstring(L, -1) || lua_isnumber(L, -1))) {
			if (!first)
				luaL_addstring(&buf, "&");
			first = 0;
			
			const char *key = lua_tostring(L, -2);
			const char *value = lua_tostring(L, -1);
			
#ifdef HAVE_CURL
			CURL *curl = curl_easy_init();
			if (curl) {
				char *enc_key = curl_easy_escape(curl, key, 0);
				char *enc_val = curl_easy_escape(curl, value, 0);
				luaL_addstring(&buf, enc_key);
				luaL_addstring(&buf, "=");
				luaL_addstring(&buf, enc_val);
				curl_free(enc_key);
				curl_free(enc_val);
				curl_easy_cleanup(curl);
			}
#else
			luaL_addstring(&buf, key);
			luaL_addstring(&buf, "=");
			luaL_addstring(&buf, value);
#endif
		}
		lua_pop(L, 1);
	}
	
	luaL_pushresult(&buf);
	return 1;
}

static int lib_http_parse_query(lua_State *L)
{
	const char *query = luaL_checkstring(L, 1);
	lua_newtable(L);
	
	char *query_copy = strdup(query);
	char *pair = strtok(query_copy, "&");
	
	while (pair != NULL) {
		char *eq = strchr(pair, '=');
		if (eq) {
			*eq = '\0';
			char *key = pair;
			char *value = eq + 1;
			
#ifdef HAVE_CURL
			CURL *curl = curl_easy_init();
			if (curl) {
				int out_len;
				char *dec_key = curl_easy_unescape(curl, key, 0, &out_len);
				char *dec_val = curl_easy_unescape(curl, value, 0, &out_len);
				lua_pushstring(L, dec_val);
				lua_setfield(L, -2, dec_key);
				curl_free(dec_key);
				curl_free(dec_val);
				curl_easy_cleanup(curl);
			}
#else
			lua_pushstring(L, value);
			lua_setfield(L, -2, key);
#endif
		}
		pair = strtok(NULL, "&");
	}
	
	free(query_copy);
	return 1;
}

static int lib_http_set_default_timeout(lua_State *L)
{
	default_timeout = luaL_checkinteger(L, 1);
	return 0;
}

static int lib_http_set_default_user_agent(lua_State *L)
{
	const char *agent = luaL_checkstring(L, 1);
	if (default_user_agent)
		free(default_user_agent);
	default_user_agent = strdup(agent);
	return 0;
}

static int lib_http_set_default_headers(lua_State *L)
{
	size_t i;
	luaL_checktype(L, 1, LUA_TTABLE);
	
	for (i = 0; i < default_header_count; i++) {
		free(default_headers[i].key);
		free(default_headers[i].value);
	}
	free(default_headers);
	default_headers = NULL;
	default_header_count = 0;
	
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
			http_header_t *tmp = realloc(default_headers, sizeof(http_header_t) * (default_header_count + 1));

			if (!tmp)
			{
				free(default_headers);
				I_Error("Out of memory");
				return 0;
			}
			else
				default_headers = tmp;

			default_headers[default_header_count].key = strdup(lua_tostring(L, -2));
			default_headers[default_header_count].value = strdup(lua_tostring(L, -1));
			default_header_count++;
		}
		lua_pop(L, 1);
	}
	
	return 0;
}

void LUA_HTTPProcessCallbacks(void)
{
	pending_callback_t *cb;
	
	while ((cb = dequeue_callback()) != NULL) {
		lua_State *L = cb->L;
		
		lua_rawgeti(L, LUA_REGISTRYINDEX, cb->callback_ref);
		
		if (cb->response) {
			push_response_table(L, cb->response);
			lua_pushnil(L);
			free_response(cb->response);
		} else {
			lua_pushnil(L);
			push_error_table(L, cb->error_msg);
			free(cb->error_msg);
		}
		
		if (lua_pcall(L, 2, 0, 0) != 0) {
			CONS_Alert(CONS_WARNING, "HTTP callback error: %s\n", lua_tostring(L, -1));
			lua_pop(L, 1);
		}

		luaL_unref(L, LUA_REGISTRYINDEX, cb->callback_ref);
		
		free(cb);
	}
}

static luaL_Reg lib_http[] = {
	{"request", lib_http_request},
	{"get", lib_http_get},
	{"post", lib_http_post},
	{"put", lib_http_put},
	{"delete", lib_http_delete},
	{"patch", lib_http_patch},
	{"encodeURL", lib_http_encode_url},
	{"decodeURL", lib_http_decode_url},
	{"encodeBase64", lib_http_encode_base64},
	{"decodeBase64", lib_http_decode_base64},
	{"buildQuery", lib_http_build_query},
	{"parseQuery", lib_http_parse_query},
	{"setDefaultTimeout", lib_http_set_default_timeout},
	{"setDefaultUserAgent", lib_http_set_default_user_agent},
	{"setDefaultHeaders", lib_http_set_default_headers},
	{NULL, NULL}
};

int LUA_HTTPLib(lua_State *L)
{
	lua_newtable(L);
	luaL_register(L, NULL, lib_http);
	lua_setglobal(L, "HTTP");
	return 0;
}
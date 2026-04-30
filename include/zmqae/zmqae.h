#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define ZMQAE_OK       0
#define ZMQAE_ERROR    -1
#define ZMQAE_TIMEOUT  -2
#define ZMQAE_INVALID  -3
#define ZMQAE_CLOSED   -4
#define ZMQAE_ALREADY  -5

typedef struct zmqae_client_s zmqae_client_t;
typedef struct zmqae_router_s zmqae_router_t;
typedef struct zmqae_perform_ctx_s zmqae_perform_ctx_t;

const char *zmqae_last_error(void);

typedef struct {
    const void *data;
    int size;
} zmqae_binary_t;

typedef void (*zmqae_perform_callback)(void *user_data,
                                        const char *id,
                                        const char *json_value,
                                        const char *error_message);

typedef void (*zmqae_handler_fn)(void *user_data, zmqae_perform_ctx_t *ctx);

zmqae_client_t *zmqae_client_new(const char *endpoint);
void zmqae_client_destroy(zmqae_client_t *client);

int zmqae_client_perform(zmqae_client_t *client,
                          const char *effect,
                          const char *json_payload,
                          zmqae_perform_callback callback,
                          void *user_data);

int zmqae_client_perform_timeout(zmqae_client_t *client,
                                  const char *effect,
                                  const char *json_payload,
                                  int timeout_ms,
                                  zmqae_perform_callback callback,
                                  void *user_data);

int zmqae_client_perform_binary(zmqae_client_t *client,
                                 const char *effect,
                                 const char *json_payload,
                                 const zmqae_binary_t *bins,
                                 int bin_count,
                                 int timeout_ms,
                                 zmqae_perform_callback callback,
                                 void *user_data);

int zmqae_client_poll(zmqae_client_t *client);

int zmqae_client_close(zmqae_client_t *client);

zmqae_router_t *zmqae_router_new(const char *endpoint);
void zmqae_router_destroy(zmqae_router_t *router);

int zmqae_router_on(zmqae_router_t *router,
                     const char *effect,
                     zmqae_handler_fn handler,
                     void *user_data);

int zmqae_router_off(zmqae_router_t *router, const char *effect);

int zmqae_router_poll(zmqae_router_t *router);

int zmqae_router_close(zmqae_router_t *router);

const char *zmqae_ctx_get_id(zmqae_perform_ctx_t *ctx);
const char *zmqae_ctx_get_effect(zmqae_perform_ctx_t *ctx);
const char *zmqae_ctx_get_payload(zmqae_perform_ctx_t *ctx);

int zmqae_ctx_binary_count(zmqae_perform_ctx_t *ctx);
int zmqae_ctx_get_binary(zmqae_perform_ctx_t *ctx, int index,
                          const void **out_data, int *out_size);

int zmqae_ctx_resume(zmqae_perform_ctx_t *ctx, const char *json_value);
int zmqae_ctx_resume_binary(zmqae_perform_ctx_t *ctx, const char *json_value,
                             const zmqae_binary_t *bins, int bin_count);
int zmqae_ctx_error(zmqae_perform_ctx_t *ctx, const char *error_message);

void zmqae_ctx_release(zmqae_perform_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

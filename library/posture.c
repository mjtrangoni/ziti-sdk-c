// Copyright (c) 2019-2022.  NetFoundry Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "posture.h"
#include <utils.h>

#if _WIN32
#include <winnt.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <lmcons.h>
#include <lmapibuf.h>
#include <lmjoin.h>

#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#include <VersionHelpers.h>
#include <windows.h>


#elif __APPLE__ && __MACH__
   #include <TargetConditionals.h>
   #if !defined(TARGET_OS_IPHONE) && !defined(TARGET_OS_SIMULATOR)
      #include <libproc.h>
   #endif
#endif

#define NANOS(s) ((s) * 1e9)
#define MILLIS(s) ((s) * 1000)

const int NO_TIMEOUTS = -1;

const bool IS_ERRORED = true;
const bool IS_NOT_ERRORED = false;

struct query_info {
    ziti_service *service;
    ziti_posture_query_set *query_set;
    ziti_posture_query *query;
};

struct pr_info_s {
    char *id;
    char *obj;
    bool should_send;
    bool pending;
    bool obsolete;
};

typedef struct pr_info_s pr_info;

struct pr_cb_ctx_s {
    ziti_context ztx;
    pr_info *info;
};

struct process_work {
    uv_work_t w;
    bool canceled;
    char *id;
    char *path;
    ziti_context ztx;
    ziti_pr_process_cb cb;

    bool is_running;
    char *sha512;
    char **signers;
    int num_signers;
};

typedef struct pr_cb_ctx_s pr_cb_ctx;


static void ziti_pr_ticker_cb(uv_timer_t *t);

static void ziti_pr_handle_mac(ziti_context ztx, const char *id, char **mac_addresses, int num_mac);

static void ziti_pr_handle_domain(ziti_context ztx, const char *id, const char *domain);

static void ziti_pr_handle_os(ziti_context ztx, const char *id, const char *os_type, const char *os_version, const char *os_build);

static void ziti_pr_handle_process(ziti_context ztx, const char *id, const char *path,
                                   bool is_running, const char *sha_512_hash, char **signers, int num_signers);

static void ziti_pr_send(ziti_context ztx);

static void ziti_pr_send_bulk(ziti_context ztx);

static void ziti_pr_send_individually(ziti_context ztx);

static bool ziti_pr_is_info_errored(ziti_context ztx, const char *id);

static void default_pq_os(ziti_context ztx, const char *id, ziti_pr_os_cb response_cb);

static void default_pq_mac(ziti_context ztx, const char *id, ziti_pr_mac_cb response_cb);

static void default_pq_domain(ziti_context ztx, const char *id, ziti_pr_domain_cb cb);

static void default_pq_process(ziti_context ztx, const char *id, const char *path, ziti_pr_process_cb cb);

static char **get_signers(const char *path, int *signers_count);

static int hash_sha512(ziti_context ztx, uv_loop_t *loop, const char *path, unsigned char **out_buf, size_t *out_len);

static bool check_running(uv_loop_t *loop, const char *path);

static void ziti_pr_free_pr_info(pr_info *info) {
    FREE(info->id);
    FREE(info->obj);
    FREE(info);
}

static void ziti_pr_free_pr_cb_ctx(pr_cb_ctx *ctx) {
    ziti_pr_free_pr_info(ctx->info);
    FREE(ctx);
}

void ziti_posture_init(ziti_context ztx, long interval_secs) {
    if (ztx->posture_checks == NULL) {
        NEWP(pc, struct posture_checks);

        pc->timer = new_ztx_timer(ztx);
        pc->previous_api_session_id = NULL;
        pc->controller_instance_id = NULL;
        pc->must_send_every_time = true;
        pc->must_send = false;

        ztx->posture_checks = pc;
    }

    if (!uv_is_active((uv_handle_t *) ztx->posture_checks->timer)) {
        uv_timer_start(ztx->posture_checks->timer, ziti_pr_ticker_cb, MILLIS(1)/*fire on startup*/, MILLIS(interval_secs));
    }
}

static void ziti_posture_checks_timer_free(uv_handle_t *handle) {
    FREE(handle);
}

void ziti_posture_checks_free(struct posture_checks *pcs) {
    if (pcs != NULL) {
        uv_close((uv_handle_t *) pcs->timer, ziti_posture_checks_timer_free);
        pcs->timer = NULL;
        model_map_clear(&pcs->responses, (_free_f) ziti_pr_free_pr_info);
        model_map_clear(&pcs->error_states, NULL);
        model_map_iter it = model_map_iterator(&pcs->active_work);
        while (it) {
            struct process_work *pwk = model_map_it_value(it);
            pwk->canceled = true;
            it = model_map_it_remove(it);
        }
        FREE(pcs->previous_api_session_id);
        FREE(pcs->controller_instance_id);
        FREE(pcs);
    }
}

static void ziti_pr_ticker_cb(uv_timer_t *t) {
    struct ziti_ctx *ztx = t->data;
    ziti_send_posture_data(ztx);
}

static pr_info *get_resp_info(ziti_context ztx, const char *id) {
    pr_info *resp = model_map_get(&ztx->posture_checks->responses, id);
    if (resp == NULL) {
        resp = calloc(1, sizeof(pr_info));
        resp->id = strdup(id);
        model_map_set(&ztx->posture_checks->responses, id, resp);
    }
    return resp;
}

void ziti_send_posture_data(ziti_context ztx) {
    if (ztx->api_session == NULL || ztx->api_session->id == NULL) {
        ZTX_LOG(DEBUG, "no api_session, can't submit posture responses");
        return;
    }

    if(ztx->api_session_state != ZitiApiSessionStateFullyAuthenticated) {
        ZTX_LOG(DEBUG, "api_session is partially authenticated, can't submit posture responses");
        return;
    }

    ZTX_LOG(VERBOSE, "starting to send posture data");
    bool new_session_id = ztx->posture_checks->previous_api_session_id == NULL || strcmp(ztx->posture_checks->previous_api_session_id, ztx->api_session->id) != 0;

    bool new_controller_instance = (ztx->posture_checks->controller_instance_id == NULL && ztx->controller.instance_id != NULL) || strcmp(ztx->posture_checks->controller_instance_id, ztx->controller.instance_id) != 0;

    if(new_controller_instance){
        ZTX_LOG(INFO, "first run or potential controller restart detected");
    }

    if (new_session_id || ztx->posture_checks->must_send_every_time || new_controller_instance) {
        ZTX_LOG(DEBUG, "posture checks must_send set to TRUE, new_session_id[%s], must_send_every_time[%s], new_controller_instance[%s]",
                new_session_id ? "TRUE" : "FALSE",
                ztx->posture_checks->must_send_every_time ? "TRUE" : "FALSE",
                new_controller_instance ? "TRUE" : "FALSE");

        ztx->posture_checks->must_send = true;
        FREE(ztx->posture_checks->previous_api_session_id);
        FREE(ztx->posture_checks->controller_instance_id);
        ztx->posture_checks->previous_api_session_id = strdup(ztx->api_session->id);
        ztx->posture_checks->controller_instance_id = strdup(ztx->controller.instance_id);
    } else {
        ZTX_LOG(DEBUG, "posture checks must_send set to FALSE, new_session_id[%s], must_send_every_time[%s], new_controller_instance[%s]",
                new_session_id ? "TRUE" : "FALSE",
                ztx->posture_checks->must_send_every_time ? "TRUE" : "FALSE",
                new_controller_instance ? "TRUE" : "FALSE");

        ztx->posture_checks->must_send = false;
    }

    NEWP(domainInfo, struct query_info);
    NEWP(osInfo, struct query_info);
    NEWP(macInfo, struct query_info);

    struct model_map processes = {NULL};

    __attribute__((unused)) const char *name;
    ziti_service *service;

    ZTX_LOG(VERBOSE, "checking posture queries on %zd service(s)", model_map_size(&ztx->services));

    //loop over the services and determine the query types that need responses
    //for process queries, save them by process path
    MODEL_MAP_FOREACH(name, service, &ztx->services) {
        if (model_map_size(&service->posture_query_map) == 0) {
            continue;
        }

        const char *policy_id;
        ziti_posture_query_set *set;
        MODEL_MAP_FOREACH(policy_id, set, &service->posture_query_map) {
            int queryIdx = 0;
            while (set->posture_queries[queryIdx] != NULL) {
                ziti_posture_query *query = set->posture_queries[queryIdx];
                if (strcmp(query->query_type, PC_MAC_TYPE) == 0) {
                    macInfo->query_set = set;
                    macInfo->query = query;
                    macInfo->service = service;
                } else if (strcmp(query->query_type, PC_DOMAIN_TYPE) == 0) {
                    domainInfo->query_set = set;
                    domainInfo->query = query;
                    domainInfo->service = service;
                } else if (strcmp(query->query_type, PC_OS_TYPE) == 0) {
                    osInfo->query_set = set;
                    osInfo->query = query;
                    osInfo->service = service;
                } else if (strcmp(query->query_type, PC_PROCESS_TYPE) == 0) {

                    void *curVal = model_map_get(&processes, query->process->path);
                    if (curVal == NULL) {
                        NEWP(newProcInfo, struct query_info);
                        newProcInfo->query_set = set;
                        newProcInfo->query = query;
                        newProcInfo->service = service;
                        model_map_set(&processes, query->process->path, newProcInfo);
                    }

                } else if (strcmp(query->query_type, PC_PROCESS_MULTI_TYPE) == 0) {
                    int processIdx = 0;
                    while (query->processes[processIdx] != NULL) {
                        ziti_process *process = query->processes[processIdx];

                        void *curVal = model_map_get(&processes, process->path);
                        if (curVal == NULL) {
                            NEWP(newProcInfo, struct query_info);
                            newProcInfo->query_set = set;
                            newProcInfo->query = query;
                            newProcInfo->service = service;

                            model_map_set(&processes, process->path, newProcInfo);
                        }
                        processIdx++;
                    }
                }
                queryIdx++;
            }
        }
    }

    // mark responses obsolete in case they were removed
    pr_info *resp;
    MODEL_MAP_FOREACH(name, resp, &ztx->posture_checks->responses) {
        if (!resp->pending && !resp->should_send) {
            resp->obsolete = true;
        }
    }

    if (domainInfo->query != NULL) {
        if (domainInfo->query->timeout == NO_TIMEOUTS) {
            ztx->posture_checks->must_send_every_time = false;
        }

        resp = get_resp_info(ztx, PC_DOMAIN_TYPE);
        resp->obsolete = false;
        if (!resp->pending) {
            resp->pending = true;
            if (ztx->opts.pq_domain_cb != NULL) {
                ztx->opts.pq_domain_cb(ztx, domainInfo->query->id, ziti_pr_handle_domain);
            } else {
                ZTX_LOG(VERBOSE, "using default %s cb for: service %s, policy: %s, check: %s", PC_DOMAIN_TYPE,
                         domainInfo->service->name, domainInfo->query_set->policy_id, domainInfo->query->id);
                default_pq_domain(ztx, domainInfo->query->id, ziti_pr_handle_domain);
            }
        }
    }

    if (macInfo->query != NULL) {
        if (macInfo->query->timeout == NO_TIMEOUTS) {
            ztx->posture_checks->must_send_every_time = false;
        }

        resp = get_resp_info(ztx, PC_MAC_TYPE);
        resp->obsolete = false;
        if (!resp->pending) {
            resp->pending = true;
            if (ztx->opts.pq_mac_cb != NULL) {
                ztx->opts.pq_mac_cb(ztx, macInfo->query->id, ziti_pr_handle_mac);
            } else {
                ZTX_LOG(VERBOSE, "using default %s cb for: service %s, policy: %s, check: %s", PC_MAC_TYPE,
                         macInfo->service->name, macInfo->query_set->policy_id, macInfo->query->id);
                default_pq_mac(ztx, macInfo->query->id, ziti_pr_handle_mac);
            }
        }
    }

    if (osInfo->query != NULL) {
        if (osInfo->query->timeout == NO_TIMEOUTS) {
            ztx->posture_checks->must_send_every_time = false;
        }
        resp = get_resp_info(ztx, PC_OS_TYPE);
        resp->obsolete = false;
        if (!resp->pending) {
            resp->pending = true;
            if (ztx->opts.pq_os_cb != NULL) {
                ztx->opts.pq_os_cb(ztx, osInfo->query->id, ziti_pr_handle_os);
            } else {
                ZTX_LOG(VERBOSE, "using default %s cb for: service %s, policy: %s, check: %s", PC_OS_TYPE,
                         osInfo->service->name, osInfo->query_set->policy_id, osInfo->query->id);
                default_pq_os(ztx, osInfo->query->id, ziti_pr_handle_os);
            }
        }
    }

    if (model_map_size(&processes) > 0) {
        const char *path;
        struct query_info *info;

        ziti_pq_process_cb proc_cb = ztx->opts.pq_process_cb;
        if (proc_cb == NULL) {
            proc_cb = default_pq_process;
            ZTX_LOG(VERBOSE, "using default cb for process queries");
        }
        MODEL_MAP_FOREACH(path, info, &processes) {
            if (info->query->timeout == NO_TIMEOUTS) {
                ztx->posture_checks->must_send_every_time = false;
            }
            resp = get_resp_info(ztx, path);
            resp->obsolete = false;
            if (!resp->pending) {
                resp->pending = true;
                proc_cb(ztx, info->query->id, path, ziti_pr_handle_process);
            }
        }
    }

    model_map_iter it = model_map_iterator(&ztx->posture_checks->responses);
    while (it) {
        resp = model_map_it_value(it);
        if (resp->obsolete) {
            ZTX_LOG(DEBUG, "removing obsolete posture resp[%s],  should_send = %s, pending = %s: %s", resp->id, resp->should_send ? "true" : "false", resp->pending ? "true" : "false", resp->obj);
            it = model_map_it_remove(it);
            ziti_pr_free_pr_info(resp);
        } else {
            it = model_map_it_next(it);
        }
    }

    model_map_clear(&processes, free);

    free(domainInfo);
    free(osInfo);
    free(macInfo);

    ziti_pr_send(ztx);
}

static void ziti_collect_pr(ziti_context ztx, const char *pr_obj_key, char *pr_obj, size_t pr_obj_len) {

    if (ztx->posture_checks == NULL) {
        ZTX_LOG(WARN, "ztx disabled, posture check obsolete id[%s]", pr_obj_key);
        free(pr_obj);
        return;
    }

    pr_info *current_info = model_map_get(&ztx->posture_checks->responses, pr_obj_key);

    if (current_info != NULL) {
        current_info->pending = false;

        bool changed = current_info->obj == NULL || strcmp(current_info->obj, pr_obj) != 0;
        if (changed) {
            FREE(current_info->obj);
            current_info->obj = pr_obj;
        } else {
            free(pr_obj);
        }

        current_info->should_send = ztx->posture_checks->must_send_every_time || ziti_pr_is_info_errored(ztx, current_info->id) || changed;
    } else {
        ZTX_LOG(WARN, "response info not found, posture check obsolete? id[%s]", pr_obj_key);
        free(pr_obj);
    }
}

static void handle_pr_resp_timer_events(ziti_context ztx, ziti_pr_response *pr_resp){
    ZTX_LOG(DEBUG, "handle_pr_resp_timer_events: starting");

    if(pr_resp != NULL && pr_resp->services != NULL) {
        ziti_service_timer **service_timer;
        for(service_timer = pr_resp->services; *service_timer != NULL; service_timer++){
            NEWP(val, bool);
            *val = true;
            ZTX_LOG(DEBUG, "handle_pr_resp_timer_events: forcing service name[%s] id[%s] with timeout[%d] timeoutRemaining[%d]", (*service_timer)->name, (*service_timer)->id, *(*service_timer)->timeout, *(*service_timer)->timeoutRemaining);
            ziti_force_service_update(ztx, (*service_timer)->id);
        }

    } else {
        ZTX_LOG(DEBUG, "handle_pr_resp_timer_events: pr_resp or pr_resp.services was null");
    }

    ZTX_LOG(DEBUG, "handle_pr_resp_timer_events: done");
}

static void ziti_pr_post_bulk_cb(ziti_pr_response *pr_resp, const ziti_error *err, void *ctx) {
    ziti_context ztx = ctx;

    ZTX_LOG(DEBUG, "ziti_pr_post_bulk_cb: starting");

    // if ztx is disabled this request is cancelled and posture_checks is cleared
    if (ztx->posture_checks) {
        if (err != NULL) {
            ZTX_LOG(ERROR, "error during bulk posture response submission (%d) %s", err->http_code, err->message);
            ztx->posture_checks->must_send = true; //error, must try again
            if (err->http_code == 404) {
                ztx->no_bulk_posture_response_api = true;
            }
        } else {
            ztx->posture_checks->must_send = false; //did not error, can skip submissions
            handle_pr_resp_timer_events(ztx, pr_resp);
            ziti_services_refresh(ztx, true);
            ZTX_LOG(DEBUG, "done with bulk posture response submission");
        }
    }

    free_ziti_pr_response_ptr(pr_resp);
}

static void ziti_pr_set_info_errored(ziti_context ztx, const char *id) {
    model_map_set(&ztx->posture_checks->error_states, id, (void *) &IS_ERRORED);
}

static void ziti_pr_set_info_success(ziti_context ztx, const char *id) {
    model_map_set(&ztx->posture_checks->error_states, id, (void *) &IS_NOT_ERRORED);
}

static bool ziti_pr_is_info_errored(ziti_context ztx, const char *id) {
    bool *is_errored = model_map_get(&ztx->posture_checks->error_states, id);
    if (is_errored == NULL) {
        return false;
    }

    return *is_errored;
}

static void ziti_pr_post_cb(ziti_pr_response *pr_resp, const ziti_error *err, void *ctx) {
    pr_cb_ctx *pr_ctx = ctx;
    ziti_context ztx = pr_ctx->ztx;

    ZTX_LOG(DEBUG, "ziti_pr_post_cb: starting");

    if (err != NULL) {
        ZTX_LOG(ERROR, "error during individual posture response submission (%d) %s - object: %s", err->http_code,
                 err->message, pr_ctx->info->obj);
        ziti_pr_set_info_errored(pr_ctx->ztx, pr_ctx->info->id);
    } else {
        ziti_pr_set_info_success(pr_ctx->ztx, pr_ctx->info->id);
        handle_pr_resp_timer_events(ztx, pr_resp);
        ziti_services_refresh(ztx, true);
        ZTX_LOG(TRACE, "done with one pr response submission, object: %s", pr_ctx->info->obj);
    }

    ziti_pr_free_pr_cb_ctx(ctx);
    free_ziti_pr_response_ptr(pr_resp);
}

static void ziti_pr_send(ziti_context ztx) {
    if (ztx->no_bulk_posture_response_api) {
        ziti_pr_send_individually(ztx);
    } else {
        ziti_pr_send_bulk(ztx);
    }
}

static void ziti_pr_send_bulk(ziti_context ztx) {
    size_t body_len = 0;
    char *body;

    bool send = false;
    __attribute__((unused)) const char *key;
    pr_info *info;
    MODEL_MAP_FOREACH(key, info, &ztx->posture_checks->responses) {
        if (info->should_send) {
            send = true;
            break;
        }
    }

    if (!send) {
        ZTX_LOG(VERBOSE, "no change in posture data, not sending");
        return; //nothing to send
    }

    string_buf_t buf;
    string_buf_init(&buf);
    string_buf_append_byte(&buf, '[');

    bool needs_comma = false;
    int obj_count = 0;
    MODEL_MAP_FOREACH(key, info, &ztx->posture_checks->responses) {
        if (info->should_send) {
            ZTX_LOG(VERBOSE, "sending posture response [%s], should_send = true: %s", info->id, info->obj);
            obj_count++;
            if (needs_comma) {
                string_buf_append_byte(&buf, ',');
            } else {
                needs_comma = true;
            }
            string_buf_append(&buf, info->obj);
            info->should_send = false;
        } else {
            ZTX_LOG(VERBOSE, "not sending posture response [%s], should_send = false, pending = %s: %s", info->id, info->pending ? "true" : "false", info->obj);
        }
    }

    string_buf_append_byte(&buf, ']');

    body = string_buf_to_string(&buf, &body_len);
    ZTX_LOG(DEBUG, "sending posture responses [%d]", obj_count);
    ZTX_LOG(TRACE, "bulk posture response: %s", body);

    ziti_pr_post_bulk(&ztx->controller, body, body_len, ziti_pr_post_bulk_cb, ztx);
    string_buf_free(&buf);
}

static void ziti_pr_send_individually(ziti_context ztx) {

    __attribute__((unused)) const char *key;
    pr_info *info;

    MODEL_MAP_FOREACH(key, info, &ztx->posture_checks->responses) {
        if (info->should_send) {
            char *body = strdup(info->obj);

            NEWP(new_info, pr_info);
            memcpy(new_info, info, sizeof(pr_info));

            new_info->id = strdup(info->id);
            new_info->obj = strdup(info->obj);

            NEWP(cb_ctx, pr_cb_ctx);
            cb_ctx->info = new_info;
            cb_ctx->ztx = ztx;

            ziti_pr_post(&ztx->controller, body, strlen(body), ziti_pr_post_cb, cb_ctx);
            info->should_send = false;
        }
    }
}


static void ziti_pr_handle_mac(ziti_context ztx, const char *id, char **mac_addresses, int num_mac) {
    size_t arr_size = sizeof(char (**));
    char **addresses = calloc((num_mac + 1), arr_size);

    memcpy(addresses, mac_addresses, (num_mac) * arr_size);

    ziti_pr_mac_req mac_req = {
            .id = (char *) id,
            .typeId = (char *) PC_MAC_TYPE,
            .mac_addresses = addresses,
    };

    size_t obj_len;
    char *obj = ziti_pr_mac_req_to_json(&mac_req, 0, &obj_len);

    ziti_collect_pr(ztx, PC_MAC_TYPE, obj, (int) obj_len);

    free(addresses);
}

static void ziti_pr_handle_domain(ziti_context ztx, const char *id, const char *domain) {
    ziti_pr_domain_req domain_req = {
            .id = (char *) id,
            .domain = (char *) domain,
            .typeId = (char *) PC_DOMAIN_TYPE,
    };

    size_t obj_len;
    char *obj = ziti_pr_domain_req_to_json(&domain_req, 0, &obj_len);

    ziti_collect_pr(ztx, PC_DOMAIN_TYPE, obj, obj_len);
}

static void ziti_pr_handle_os(ziti_context ztx, const char *id, const char *os_type, const char *os_version, const char *os_build) {
    ziti_pr_os_req os_req = {
            .id = (char *) id,
            .typeId = (char *) PC_OS_TYPE,
            .type = (char *) os_type,
            .version = (char *) os_version,
            .build = (char *) os_build
    };

    size_t obj_len;
    char *obj = ziti_pr_os_req_to_json(&os_req, 0, &obj_len);

    ziti_collect_pr(ztx, PC_OS_TYPE, obj, obj_len);
}


static void ziti_pr_handle_process(ziti_context ztx, const char *id, const char *path,
                                   bool is_running, const char *sha_512_hash, char **signers,
                                   int num_signers) {

    size_t arr_size = sizeof(char (**));
    char **null_term_signers = calloc((num_signers + 1), arr_size);
    memcpy(null_term_signers, signers, num_signers * arr_size);

    ziti_pr_process_req process_req = {
            .id = (char *) id,
            .path = (char *) path,
            .typeId = (char *) PC_PROCESS_TYPE,
            .is_running = is_running,
            .hash = (char *) sha_512_hash,
            .signers = null_term_signers,
    };

    size_t obj_len;
    char *obj = ziti_pr_process_req_to_json(&process_req, 0, &obj_len);

    free(null_term_signers);

    ziti_collect_pr(ztx, path, obj, obj_len);
}

#if _WIN32
typedef NTSTATUS (NTAPI *sRtlGetVersion)
        (PRTL_OSVERSIONINFOW lpVersionInformation);

static sRtlGetVersion get_win32_version_f() {
    static sRtlGetVersion s_func;
    if (s_func == NULL) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        s_func = (sRtlGetVersion) GetProcAddress(ntdll, "RtlGetVersion");
    }
    return s_func;
}
static

#endif // _WIN32

void default_pq_os(ziti_context ztx, const char *id, ziti_pr_os_cb response_cb) {
    const char *os;
    const char *ver;
    const char *build;
#if _WIN32
    OSVERSIONINFOEXW os_info = {0};
    os_info.dwOSVersionInfoSize = sizeof(os_info);
    sRtlGetVersion version_f = get_win32_version_f();
    if (version_f) {
        version_f((PRTL_OSVERSIONINFOW) &os_info);
    } else {
        /* Silence GetVersionEx() deprecation warning. */
#pragma warning(suppress : 4996)
        GetVersionExW((LPOSVERSIONINFOW) &os_info);
    }

    switch (os_info.wProductType) {
        case 1:
            os = "windows";
            break;
        case 2:
        case 3:
            os = "windowsserver";
            break;
        default:
            os = "<unknown windows type>";
    }
    char winver[16];
    sprintf_s(winver, 16, "%d.%d.%d", os_info.dwMajorVersion, os_info.dwMinorVersion, os_info.dwBuildNumber);
    ver = winver;
    build = "ununsed";
#else
    uv_utsname_t uname;
    uv_os_uname(&uname);

    os = uname.sysname;
    ver = uname.release;
    build = uname.version;
#endif

    response_cb(ztx, id, os, ver, build);
}

static bool non_zero_addr(const char *addr, int addr_size) {
    for (int i = 0; i < addr_size; i++) {
        if (addr[i] != 0) return true;
    }
    return false;
}

static void default_pq_mac(ziti_context ztx, const char *id, ziti_pr_mac_cb response_cb) {

    uv_interface_address_t *info;
    int count;
    uv_interface_addresses(&info, &count);

    model_map addrs = {0};

    int addr_size = sizeof(info[0].phys_addr);
    for (int i = 0; i < count; i++) {
        if (!info[i].is_internal && non_zero_addr(info[i].phys_addr, addr_size)) {
            if (model_map_get(&addrs, info[i].name) == NULL) {
                char *mac;
                hexify((const uint8_t *) info[i].phys_addr, addr_size, ':', &mac);
                model_map_set(&addrs, info[i].name, mac);
            }
        }
    }

    size_t addr_count = model_map_size(&addrs);
    char **addresses = calloc(addr_count, sizeof(char *));
    const char *ifname;
    char *mac;
    int idx = 0;
    MODEL_MAP_FOREACH(ifname, mac, &addrs) {
        addresses[idx++] = mac;
    }

    response_cb(ztx, id, addresses, (int) addr_count);
    free(addresses);
    model_map_clear(&addrs, free);
    uv_free_interface_addresses(info, count);
}


static void default_pq_domain(ziti_context ztx, const char *id, ziti_pr_domain_cb cb) {
#if _WIN32
    uint32_t status;
    LPWSTR buf;
    NetGetJoinInformation(NULL, &buf, &status);
    char domain[256];
    sprintf_s(domain, sizeof(domain), "%ls", buf);
    cb(ztx, id, domain);
    NetApiBufferFree(buf);
#else
    cb(ztx, id, "");
#endif
}

static void process_check_work(uv_work_t *w);

static void process_check_done(uv_work_t *w, int status) {
    struct process_work *pcw = container_of(w, struct process_work, w);
    if (!pcw->canceled) {
        model_map_remove_key(&pcw->ztx->posture_checks->active_work, &pcw, sizeof(uintptr_t));
        pcw->cb(pcw->ztx, pcw->id, pcw->path, pcw->is_running, pcw->sha512, pcw->signers, pcw->num_signers);
    } else {
        ZITI_LOG(INFO, "process check path[%s] was cancelled", pcw->path);
    }
    free(pcw->id);
    free(pcw->path);
    FREE(pcw->sha512);
    if (pcw->signers) {
        for (int i = 0; i < pcw->num_signers; i++) {
            free(pcw->signers[i]);
        }
        free(pcw->signers);
    }
    free(pcw);
}

bool ziti_service_has_query_with_timeout(ziti_service *service) {
    ziti_posture_query_set *current_set = NULL;
    model_map_iter it = model_map_iterator(&service->posture_query_map);
    while (it != NULL) {
        current_set = model_map_it_value(it);

        ziti_posture_query *current_query = current_set->posture_queries[0];
        for (int posture_query_idx = 1; current_query != NULL; posture_query_idx++) {

            if (current_query->timeout != NO_TIMEOUTS) {
                return true;
            }

            current_query = current_set->posture_queries[posture_query_idx];
        }

        it = model_map_it_remove(it);
    }

    return false;
}

static void default_pq_process(ziti_context ztx, const char *id, const char *path, ziti_pr_process_cb cb) {
    NEWP(wr, struct process_work);
    wr->id = strdup(id);
    wr->path = strdup(path);
    wr->cb = cb;
    wr->ztx = ztx;
    model_map_set_key(&ztx->posture_checks->active_work, &wr, sizeof(uintptr_t), wr);
    uv_queue_work(ztx->loop, &wr->w, process_check_work, process_check_done);
}

static void process_check_work(uv_work_t *w) {
    struct process_work *pcw = container_of(w, struct process_work, w);
    ziti_context ztx = pcw->ztx;
    const char *path = pcw->path;

    unsigned char *digest;
    size_t digest_len;
    uv_fs_t file;
    int rc = uv_fs_stat(w->loop, &file, path, NULL);
    if (rc != 0) {
        return;
    }

    pcw->is_running = check_running(w->loop, path);
    if (hash_sha512(ztx, w->loop, path, &digest, &digest_len) == 0) {
        hexify(digest, digest_len, 0, &pcw->sha512);
        ZITI_LOG(VERBOSE, "file(%s) hash = %s", path, pcw->sha512);
        free(digest);
    }
    pcw->signers = get_signers(path, &pcw->num_signers);
}

void ziti_endpoint_state_pr_cb(ziti_pr_response *pr_resp, const ziti_error *err, void *ctx) {
    ziti_context ztx = ctx;
    if (err) {
        ZTX_LOG(ERROR, "error during endpoint state posture response submission: %d - %s", err->http_code, err->message);
    } else {
        ZTX_LOG(INFO, "endpoint state sent");
        handle_pr_resp_timer_events(ztx, pr_resp);
        ziti_services_refresh(ztx, true);
    }
    free_ziti_pr_response_ptr(pr_resp);
}


void ziti_endpoint_state_change(ziti_context ztx, bool woken, bool unlocked) {
    if (woken || unlocked) {
        ZTX_LOG(INFO, "endpoint state change reported: woken[%s] unlocked[%s]", woken ? "TRUE":"FALSE", unlocked ? "TRUE":"FALSE");
        ziti_pr_endpoint_state_req state_req = {
                .id = "0",
                .typeId = (char *) PC_ENDPOINT_STATE_TYPE,
                .unlocked = unlocked,
                .woken = woken
        };

        size_t obj_len;

        char *obj = ziti_pr_endpoint_state_req_to_json(&state_req, 0, &obj_len);

        ziti_pr_post(&ztx->controller, obj, obj_len, ziti_endpoint_state_pr_cb, ztx);
    } else {
        ZTX_LOG(INFO, "endpoint state change reported, but no reason to send data: woken[%s] unlocked[%s]", woken ? "TRUE":"FALSE", unlocked ? "TRUE":"FALSE");
    }
}


static int hash_sha512(ziti_context ztx, uv_loop_t *loop, const char *path, unsigned char **out_buf, size_t *out_len) {
    size_t digest_size = crypto_hash_sha512_bytes();
    unsigned char *digest = NULL;
    int rc = 0;

#define CHECK(op) do{ rc = (op); if (rc != 0) { \
ZITI_LOG(ERROR, "failed hashing path[%s] op[" #op "]: %d", path, rc); \
goto cleanup;                                   \
} }while(0)

    uv_fs_t ft;
    uv_file file = uv_fs_open(loop, &ft, path, UV_FS_O_RDONLY, 0, NULL);

    if (file < 0) { return -1; }
    uv_buf_t buf = uv_buf_init(malloc(64 * 1024), 64 * 1024);
    int64_t offset = 0;
    crypto_hash_sha512_state sha512;
    crypto_hash_sha512_init(&sha512);

    // hash data
    while (true) {
        int read = uv_fs_read(loop, &ft, file, &buf, 1, offset, NULL);
        if (read == 0) {
            break;
        }

        if (read < 0) {
            rc = -1;
            goto cleanup;
        }

        offset += read;
        CHECK(crypto_hash_sha512_update(&sha512, (uint8_t *) buf.base, read));
    }
    digest = malloc(digest_size);
    CHECK(crypto_hash_sha512_final(&sha512, digest));

    *out_buf = digest;
    *out_len = digest_size;

    cleanup:
    if (rc != 0) FREE(digest);
    uv_fs_close(loop, &ft, file, NULL);
    uv_fs_req_cleanup(&ft);
    FREE(buf.base);

    return rc;
}

static bool check_running(uv_loop_t *loop, const char *path) {
    bool result = false;
#if _WIN32
    HANDLE sh = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (sh == INVALID_HANDLE_VALUE) {
        ZITI_LOG(ERROR, "failed to get process list: %lu", GetLastError());
        return result;
    }

    // Set the size of the structure before using it.
    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(pe32);
    // Retrieve information about the first process, and exit if unsuccessful
    if( !Process32First( sh, &pe32 ) )
    {
        CloseHandle( sh );          // clean the snapshot object
        return( FALSE );
    }

    char fullPath[1024];
    DWORD fullPathSize;

    // Now walk the snapshot of processes, and display information about each process in turn
    ZITI_LOG(VERBOSE, "checking to see if process is running: %s", path);
    do
    {
        ZITI_LOG(VERBOSE, "process is running: %s", pe32.szExeFile);

        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe32.th32ProcessID);
        if (ph == NULL) {
            if (pe32.th32ProcessID > 0) {
                ZITI_LOG(DEBUG, "process %s is running, however not able to open handle. GetLastError(): %lu", pe32.szExeFile, GetLastError());
            }
            continue;
        }
        fullPathSize = sizeof(fullPath);
        QueryFullProcessImageNameA(ph, 0, fullPath, &fullPathSize);

        ZITI_LOG(VERBOSE, "comparing process: %s to: %.*s", pe32.szExeFile, fullPathSize, fullPath);
        if (strnicmp(path, fullPath, fullPathSize) == 0) {
            result = true;
            break;
        }
    } while( Process32Next( sh, &pe32 ) );

    CloseHandle(sh);

#elif __linux || __linux__

    uv_fs_t fs_proc;
    uv_fs_scandir(loop, &fs_proc, "/proc", 0, NULL);
    uv_dirent_t de;
    uv_fs_t ex;
    char proc_path[128];
    while (!result && uv_fs_scandir_next(&fs_proc, &de) != UV_EOF) {
        if (de.type == UV_DIRENT_DIR) {
            snprintf(proc_path, sizeof(proc_path), "/proc/%s/exe", de.name);
            if (uv_fs_readlink(loop, &ex, proc_path, NULL) == 0) {
                if (strcmp((const char *) ex.ptr, path) == 0) {
                    result = true;
                }
                free(ex.ptr);
            }
        }
    }
    uv_fs_req_cleanup(&fs_proc);

#elif __APPLE__ && __MACH__ && !defined(TARGET_OS_IPHONE) && !defined(TARGET_OS_SIMULATOR)
    int n_pids = proc_listallpids(NULL, 0);
    unsigned long pids_sz = sizeof(pid_t) * (unsigned long)n_pids;
    pid_t * pids = calloc(1, pids_sz);
    proc_listallpids(pids, (int)pids_sz);
    char proc_path[PROC_PIDPATHINFO_MAXSIZE];
    for (int i=0; i < n_pids; i++) {
        if (pids[i] == 0) continue;
        proc_pidpath(pids[i], proc_path, sizeof(proc_path)); // returns strlen(proc_path)
        if (strncasecmp(proc_path, path, sizeof(proc_path)) == 0) {
            result = true;
            break;
        }
    }
    free(pids);
#else
    uv_utsname_t uname;
    uv_os_uname(&uname);
    ZITI_LOG(WARN, "not implemented on %s", uname.sysname);
#endif
    ZITI_LOG(DEBUG, "is running result: %s for %s", (result ? "true" : "false"), path);
    return result;
}

char **get_signers(const char *path, int *signers_count) {
    char **result = NULL;
#if _WIN32
    WCHAR filename[MAX_PATH];
    HCERTSTORE hStore = NULL;
    HCRYPTMSG hMsg = NULL;
    PCCERT_CONTEXT pCertContext = NULL;
    BOOL res;
    DWORD dwEncoding, dwContentType, dwFormatType;

    size_t conv;
    mbstowcs_s(&conv, filename, MAX_PATH, path, MAX_PATH);

    res = CryptQueryObject(CERT_QUERY_OBJECT_FILE,
                           filename,
                           CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                           CERT_QUERY_FORMAT_FLAG_BINARY,
                           0,
                           &dwEncoding,
                           &dwContentType,
                           &dwFormatType,
                           &hStore,
                           &hMsg,
                           NULL);

    if (!res) return NULL;

    result = calloc(16, sizeof(char *));
    int idx = 0;
    pCertContext = CertEnumCertificatesInStore(hStore, NULL);
    while (pCertContext != NULL) {
        BYTE sha1[20];
        char *hex;
        DWORD size = sizeof(sha1);
        BOOL rc = CertGetCertificateContextProperty(pCertContext, CERT_SHA1_HASH_PROP_ID, sha1, &size);
        if (!rc) {
            ZITI_LOG(WARN, "failed to get cert[%d] sig: %lu", idx, GetLastError());
            continue;
        } else {
            hexify(sha1, sizeof(sha1), 0, &hex);
            ZITI_LOG(VERBOSE, "%s cert[%d] sig = %s", path, idx, hex);

        }
        pCertContext = CertEnumCertificatesInStore(hStore, pCertContext);
        result[idx++] = hex;
    }
    *signers_count = idx;

#else
    *signers_count = 0;
#endif
    return result;
}

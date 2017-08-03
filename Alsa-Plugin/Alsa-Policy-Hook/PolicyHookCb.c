/*
 * Copyright (C) 2016 "IoT.bzh"
 * Author Fulup Ar Foll <fulup@iot.bzh>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * AfbCallBack (snd_ctl_hal_t *handle, int numid, void **response);
 *  AlsaHookInit is mandatory and called with numid=0
 * 
 * Syntax in .asoundrc file
 *   CrlLabel    { cb MyFunctionName name "My_Second_Control" }
 * 
 * Testing:
 *   aplay -DAlsaHook /usr/share/sounds/alsa/test.wav 
 * 
 * References:
 *  https://www.spinics.net/lists/alsa-devel/msg54235.html
 *  https://github.com/shivdasgujare/utilities/blob/master/nexuss/alsa-scenario-hook/src/alsa-wrapper.c
 */

#define _GNU_SOURCE  
#include <stdio.h>
#include <alsa/asoundlib.h>
#include <alsa/conf.h>
#include <alsa/pcm.h>

#include <systemd/sd-event.h>
#include <json-c/json.h>

#include "afb/afb-wsj1.h"
#include "afb/afb-ws-client.h"
#include <pthread.h>
#include <semaphore.h>


#define PLUGIN_ENTRY_POINT AlsaInstallHook
    // Fulup Note: What ever you may find on Internet you should use
    // SND_CONFIG_DLSYM_VERSION_HOOK and not SND_CONFIG_DLSYM_VERSION_HOOK
    SND_DLSYM_BUILD_VERSION(PLUGIN_ENTRY_POINT, SND_PCM_DLSYM_VERSION)

// this should be more than enough
#define MAX_API_CALL 10
    
// timeout in ms    
#define REQUEST_DEFAULT_TIMEOUT 100
#ifndef MAINLOOP_WATCHDOG
#define MAINLOOP_WATCHDOG 60000
#endif

// Currently not implemented
#define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#define UNUSED_FUNCTION(x) __attribute__((__unused__)) UNUSED_ ## x
void OnRequestCB(void* UNUSED(handle) , const char* UNUSED(api), const char* UNUSED(verb), struct afb_wsj1_msg*UNUSED(msg)) {}

    typedef struct {
    snd_pcm_t *pcm;
    const char *uri;
    struct afb_wsj1 *wsj1;
    sd_event *sdLoop;
    int verbose;
    sem_t semaphore;
    int count;
    int error;
} afbClientT;

typedef struct {
    const char *api;
    const char *verb;
    long timeout;
    char *query;
    
    sd_event_source *evtSource;
    char *callIdTag;
    afbClientT *afbClient;
} afbRequestT;

static void *LoopInThread(void *handle) {
    afbClientT *afbClient = (afbClientT*) handle;
    int count=0;
    int watchdog= MAINLOOP_WATCHDOG *1000;

    /* loop until end */
    for (;;) {
        
        if (afbClient->verbose) printf("ON-MAINLOOP Active count=%d\n", count++);
        sd_event_run(afbClient->sdLoop, watchdog);
    }

    return NULL;
}

// lost connect with the AudioDaemon
static void OnHangupCB(void *handle, struct afb_wsj1 *wsj1) {

    afbClientT *afbClient = (afbClientT*) handle;
    SNDERR("(Hoops) Lost Connection to %s", afbClient->uri);
    
    // try to close PCM when impossible terminate client
    int err = snd_pcm_close (afbClient->pcm);
    if (err) exit(1);
}

// supported action
typedef enum {
    ACTION_PAUSE,
    ACTION_START,
    ACTION_STOP,
    ACTION_RESUME,
            
    ACTION_NONE
} evtActionEnumT;

// action label/enum map
const char *evtActionLabels[] ={
    [ACTION_PAUSE] = "pause",
    [ACTION_START] = "start",
    [ACTION_STOP]  = "stop",
    [ACTION_RESUME]= "resume",
    
    [ACTION_NONE]=NULL
};

static evtActionEnumT getActionEnum (const char *label) {
    int idx;
    
    if (!label) return ACTION_NONE;
    
    for (idx = 0; evtActionLabels[idx] != NULL; idx++) {
        if (! strcmp(evtActionLabels[idx], label)) break;
    }
    
    if (evtActionLabels[idx]) return idx;
    else return ACTION_NONE;
}

void OnEventCB(void *handle, const char *event, struct afb_wsj1_msg *msg) {
    afbClientT *afbClient = (afbClientT*) handle;
    json_object *eventJ, *tmpJ, *dataJ;
    const char *action;
    int err, value, done;
    
    
    eventJ = afb_wsj1_msg_object_j(msg);
    done= json_object_object_get_ex(eventJ,"data", &dataJ);
    if (!done) {
        SNDERR ("PCM_HOOK: uri=%s empty event action", afbClient->uri);  
        goto OnErrorExit;
    }   

    json_object_object_get_ex(dataJ,"action", &tmpJ);
    action=json_object_get_string(tmpJ);
    
    json_object_object_get_ex(dataJ,"value", &tmpJ);
    value=json_object_get_int(tmpJ);
    
    switch (getActionEnum(action)) {
        case ACTION_PAUSE:
            err = snd_pcm_pause (afbClient->pcm, value);
            if (err < 0) SNDERR ("PCM_HOOK: Fail to pause value=%d\n", value);
            break;
            
        case ACTION_STOP:    
            err = snd_pcm_drop (afbClient->pcm);
            if (err < 0) SNDERR ("PCM_HOOK: Fail to close\n");
            break;
            
        case ACTION_START:    
            err = snd_pcm_resume (afbClient->pcm);
            if (err < 0) SNDERR ("PCM_HOOK: Fail to start\n");
            break;
            
        default:
            SNDERR ("PCM_HOOK: Unsupported Event uri=%s action=%s", afbClient->uri, action);         
            goto OnErrorExit;
    }
    
    if (afbClient->verbose) printf("ON-EVENT action=%s value=%d\n", action, value);
    return;
    
OnErrorExit:
    SNDERR("ON-EVENT %s(%s)\n", event, afb_wsj1_msg_object_s(msg));
    return;
}

// callback interface for wsj1
static struct afb_wsj1_itf itf = {
    .on_hangup = OnHangupCB,
    .on_call = OnRequestCB,
    .on_event = OnEventCB
};

void OnResponseCB(void *handle, struct afb_wsj1_msg *msg) {
    afbRequestT *afbRequest= (afbRequestT*)handle;

    if (afbRequest->afbClient->verbose) printf("ON-RESPONSE call=%s response=%s\n", afbRequest->callIdTag, afb_wsj1_msg_object_s(msg));
    
    // Cancel timeout for this request
    sd_event_source_unref(afbRequest->evtSource);
    
    if (! afb_wsj1_msg_is_reply_ok(msg)) goto OnErrorExit;
    
    // When not more waiting call release semaphore
    afbRequest->afbClient->count--;
    if (afbRequest->afbClient->count == 0) {
        if (afbRequest->afbClient->verbose) printf("ON-RESPONSE No More Waiting Request\n");
        afbRequest->afbClient->error=0;
        sem_post (&afbRequest->afbClient->semaphore);
    }
    return;

OnErrorExit:    
    fprintf(stderr, "ON-RESPONSE ERROR call=%s response=%s\n", afbRequest->callIdTag, afb_wsj1_msg_object_s(msg));
    afbRequest->afbClient->error=1;
    sem_post (&afbRequest->afbClient->semaphore);
}

int OnTimeoutCB (sd_event_source* source, uint64_t timer, void* handle) {
    afbClientT *afbClient= (afbClientT*)handle;

    SNDERR("\nON-TIMEOUT Call Request Fail URI=%s\n", afbClient->uri);
    
    // Close PCM and release waiting client
    afbClient->error=1;
    sem_post (&afbClient->semaphore);          
    
    return 0;
}

// Call AGL binder asynchronously by with a timeout
static int CallWithTimeout(afbClientT *afbClient, afbRequestT *afbRequest, int count) {
    uint64_t usec;
    int err;

    // create a unique tag for request
    (void) asprintf(&afbRequest->callIdTag, "%d:%s/%s", count, afbRequest->api, afbRequest->verb);
    
    // create a timer with ~250us accuracy
    sd_event_now(afbClient->sdLoop, CLOCK_MONOTONIC, &usec);
    sd_event_add_time(afbClient->sdLoop, &afbRequest->evtSource, CLOCK_MONOTONIC, usec+afbRequest->timeout*1000, 250, OnTimeoutCB, afbClient);

    err = afb_wsj1_call_s(afbClient->wsj1, afbRequest->api, afbRequest->verb, afbRequest->query, OnResponseCB, afbRequest);
    if (err) goto OnErrorExit;
    
    // save client handle in request
    afbRequest->afbClient = afbClient;
    afbClient->count ++;
    
    return 0;
    
OnErrorExit:
    return -1;
}

static int LaunchCallRequest(afbClientT *afbClient, afbRequestT **afbRequest) {

    pthread_t tid;
    int err, idx;

    // init waiting counting semaphore
    if (sem_init(&afbClient->semaphore, 1, 0) == -1) {
       fprintf(stderr, "LaunchCallRequest: Fail Semaphore Init: %s\n", afbClient->uri); 
    }
    
    // Create a main loop
    err = sd_event_default(&afbClient->sdLoop);
    if (err < 0) {
        fprintf(stderr, "LaunchCallRequest: Connection to default event loop failed: %s\n", strerror(-err));
        goto OnErrorExit;
    }
    
    // start a thread with a mainloop to monitor Audio-Agent
    err = pthread_create(&tid, NULL, &LoopInThread, afbClient);
    if (err) goto OnErrorExit;
    
    // connect the websocket wsj1 to the uri given by the first argument 
    afbClient->wsj1 = afb_ws_client_connect_wsj1(afbClient->sdLoop, afbClient->uri, &itf, afbClient);
    if (afbClient->wsj1 == NULL) {
        fprintf(stderr, "LaunchCallRequest: Connection to %s failed\n", afbClient->uri);
        goto OnErrorExit;
    }

    // send call request to audio-agent asynchronously (respond with thread mainloop context)
    for (idx = 0; afbRequest[idx] != NULL; idx++) {
        err = CallWithTimeout(afbClient, afbRequest[idx], idx);
        if (err) {
            fprintf(stderr, "LaunchCallRequest: Fail call %s//%s/%s&%s", afbClient->uri, afbRequest[idx]->api, afbRequest[idx]->verb, afbRequest[idx]->query);
            goto OnErrorExit;
        }        
    }
    
    // launch counter to keep track of waiting request call
    afbClient->count=idx;

    return 0;

OnErrorExit:
    return -1;
}

static int AlsaCloseHook(snd_pcm_hook_t *hook) {

    afbClientT *afbClient = (afbClientT*) snd_pcm_hook_get_private (hook);
    
    if (afbClient->verbose) fprintf(stdout, "\nAlsaCloseHook pcm=%s\n", snd_pcm_name(afbClient->pcm));
    return 0;
}

// Function call when Plugin PCM is OPEN
int PLUGIN_ENTRY_POINT (snd_pcm_t *pcm, snd_config_t *conf) {
    afbRequestT **afbRequest;
    snd_pcm_hook_t *h_close = NULL;
    snd_config_iterator_t it, next;
    afbClientT *afbClient = malloc(sizeof (afbClientT));
    afbRequest = malloc(MAX_API_CALL * sizeof (afbRequestT));
    int err;

    // start populating client handle
    afbClient->pcm = pcm;
    afbClient->verbose = 0;

    
    // Get PCM arguments from asoundrc
    snd_config_for_each(it, next, conf) {
        snd_config_t *node = snd_config_iterator_entry(it);
        const char *id;

        // ignore comment en empty lines
        if (snd_config_get_id(node, &id) < 0) continue;
        if (strcmp(id, "comment") == 0 || strcmp(id, "hint") == 0) continue;

        if (strcmp(id, "uri") == 0) {
            const char *uri;
            if (snd_config_get_string(node, &uri) < 0) {
                SNDERR("Invalid String for %s", id);
                goto OnErrorExit;
            }
            afbClient->uri=strdup(uri);
            continue;
        }

        if (strcmp(id, "verbose") == 0) {
            afbClient->verbose= snd_config_get_bool(node);
            if (afbClient->verbose < 0) {
                SNDERR("Invalid Boolean for %s", id);
                goto OnErrorExit;
            }
            continue;
        }

        if (strcmp(id, "request") == 0) {
            const char *callConf, *callLabel;
            snd_config_type_t ctype;
            snd_config_iterator_t currentCall, follow;
            snd_config_t *itemConf;
            int callCount=0;

            ctype = snd_config_get_type(node);
            if (ctype != SND_CONFIG_TYPE_COMPOUND) {
                snd_config_get_string(node, &callConf);
                SNDERR("Invalid compound type for %s", callConf);
                goto OnErrorExit;
            }


            // loop on each call 
            snd_config_for_each(currentCall, follow, node) {
                snd_config_t *ctlconfig = snd_config_iterator_entry(currentCall);
                
                // ignore empty line
                if (snd_config_get_id(ctlconfig, &callLabel) < 0) continue;

                // each clt should be a valid config compound
                ctype = snd_config_get_type(ctlconfig);
                if (ctype != SND_CONFIG_TYPE_COMPOUND) {
                    snd_config_get_string(node, &callConf);
                    SNDERR("Invalid call element for %s", callLabel);
                    goto OnErrorExit;
                }

                // allocate an empty call request
                afbRequest[callCount] = calloc(1, sizeof (afbRequestT));


                err = snd_config_search(ctlconfig, "api", &itemConf);
                if (!err) {
                    if (snd_config_get_string(itemConf, &afbRequest[callCount]->api) < 0) {
                        SNDERR("Invalid api string for %s", callLabel);
                        goto OnErrorExit;
                    }
                }
                
                err = snd_config_search(ctlconfig, "verb", &itemConf);
                if (!err) {
                    if (snd_config_get_string(itemConf, &afbRequest[callCount]->verb) < 0) {
                        SNDERR("Invalid verb string %s", id);
                        goto OnErrorExit;
                    }
                }
                
                err = snd_config_search(ctlconfig, "timeout", &itemConf);
                if (!err) {
                    if (snd_config_get_integer(itemConf, &afbRequest[callCount]->timeout) < 0) {
                        SNDERR("Invalid timeout Integer %s", id);
                        goto OnErrorExit;
                    }
                }
                
                err = snd_config_search(ctlconfig, "query", &itemConf);
                if (!err) {
                    const char *query;
                    if (snd_config_get_string(itemConf, &query) < 0) {
                        SNDERR("Invalid query string %s", id);
                        goto OnErrorExit;
                    }
                    // cleanup string for json_tokener
                    afbRequest[callCount]->query = strdup(query);
                    for (int idx = 0; query[idx] != '\0'; idx++) {
                        if (query[idx] == '\'') afbRequest[callCount]->query[idx] = '"';
                        else afbRequest[callCount]->query[idx] = query[idx];
                    }
                    json_object *queryJ = json_tokener_parse(query);
                    if (!queryJ) {
                        SNDERR("Invalid Json %s query=%s format \"{'tok1':'val1', 'tok2':'val2'}\" ", id, query);
                        goto OnErrorExit;
                    }
                }
                
                // Simple check on call request validity
                if (!afbRequest[callCount]->query)   afbRequest[callCount]->query= "";
                if (!afbRequest[callCount]->timeout) afbRequest[callCount]->timeout=REQUEST_DEFAULT_TIMEOUT ;
                if (!afbRequest[callCount]->verb || !afbRequest[callCount]->api) {
                    SNDERR("Missing api/verb %s in asoundrc", callLabel);
                    goto OnErrorExit;
                }

                // move to next call if any
                callCount ++;
                if (callCount == MAX_API_CALL) {
                    SNDERR("Too Many call MAX_API_CALL=%d", MAX_API_CALL);
                    goto OnErrorExit;                    
                }
                afbRequest[callCount]=NULL; // afbRequest array is NULL terminated

            } 
            continue;
        }
    }
    
    if (afbClient->verbose) fprintf(stdout, "\nAlsaHook Install Start PCM=%s URI=%s\n", snd_pcm_name(afbClient->pcm), afbClient->uri);

    err = snd_pcm_hook_add(&h_close, afbClient->pcm, SND_PCM_HOOK_TYPE_CLOSE, AlsaCloseHook, afbClient);
    if (err < 0) goto OnErrorExit;

    // launch call request and create a waiting mainloop thread
    err = LaunchCallRequest(afbClient, afbRequest);
    if (err < 0) {
        fprintf (stderr, "PCM Fail to Enter Mainloop\n");
        goto OnErrorExit;
    }
    
    // wait for all call request to return
    sem_wait(&afbClient->semaphore);
    if (afbClient->error) {
        fprintf (stderr, "PCM Authorisation from Audio Agent Refused\n");
        goto OnErrorExit;        
    }

    if (afbClient->verbose) fprintf(stdout, "\nAlsaHook Install Success PCM=%s URI=%s\n", snd_pcm_name(afbClient->pcm), afbClient->uri);
    return 0;

OnErrorExit:
    fprintf(stderr, "\nAlsaPcmHook Plugin Install Fail PCM=%s\n", snd_pcm_name(afbClient->pcm));
    if (h_close)
        snd_pcm_hook_remove(h_close);

    return -EINVAL;
}

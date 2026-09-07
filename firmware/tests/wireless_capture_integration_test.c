// Actual capture + wireless lifecycle + binary framing in one harness.
// Deterministic driver/RTOS/JSON/network shims are NOT physical qualification.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "integrated_stubs/platform.h"
#define TAG CAPTURE_TAG
#include "../main/voice_audio.c"
#undef TAG
#include "../main/wireless_microphone.c"
#include "../main/wireless_microphone_protocol.c"

struct fake_queue { unsigned capacity, count, head; size_t width; uint8_t *data; };
static struct fake_queue queue_store;
static struct fake_semaphore locks[8];
static unsigned lock_count;
static EventBits_t event_bits;
static int64_t time_us;
static unsigned codec_reads, binary_sends, stop_calls, start_calls, cancel_sends, hello_sends;
static wifi_ps_type_t ps;
static bool fail_allocation, fail_capture_task, fail_binary, handshake, stop_ack;
static bool omit_armed, fail_gate, revoke_before_arm, no_codec, run_stream;
static bool in_callback, check_mutexes;
static unsigned stream_send_limit, send_tick;
static void (*during_read)(void), (*during_reset)(void), (*during_wait)(void), (*before_power)(void);
static jmp_buf capture_exit, stream_exit;
static cJSON *fixture;
static void capture_one(void);
static void stream_one(void);
static void deliver(const char *, uint32_t);
static void prepare(void);

static char *dup_string(const char *s) { size_t n = strlen(s)+1; char *p=malloc(n); assert(p); memcpy(p,s,n); return p; }
size_t strlcpy(char *d, const char *s, size_t cap) { size_t n=strlen(s); if(cap){size_t m=n<cap-1?n:cap-1; memcpy(d,s,m); d[m]=0;} return n; }
cJSON *cJSON_CreateObject(void) { cJSON *v=calloc(1,sizeof(*v)); assert(v); v->type=1; return v; }
void cJSON_Delete(cJSON *v) { if(!v)return; cJSON_Delete(v->child); cJSON_Delete(v->next); free(v->valuestring); free(v); }
void cJSON_free(void *p) { free(p); }
static cJSON *field(cJSON *o,const char *k) { cJSON *v=cJSON_CreateObject(); v->key=k; v->next=o->child; o->child=v; return v; }
void cJSON_AddStringToObject(cJSON *o,const char *k,const char *s) { cJSON *v=field(o,k); v->type=2; v->valuestring=dup_string(s); }
void cJSON_AddNumberToObject(cJSON *o,const char *k,double n) { cJSON *v=field(o,k); v->type=3; v->valuedouble=n; }
const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *o,const char *k) { if(o)for(cJSON *v=o->child;v;v=v->next)if(!strcmp(k,v->key))return v; return NULL; }
bool cJSON_IsString(const cJSON *v) { return v&&v->type==2; }
bool cJSON_IsNumber(const cJSON *v) { return v&&v->type==3; }
bool cJSON_IsObject(const cJSON *v) { return v&&v->type==1; }
cJSON *cJSON_ParseWithLength(const char *s,size_t n) { assert(n==7&&!memcmp(s,"fixture",7)&&fixture); cJSON *v=fixture; fixture=NULL; return v; }
char *cJSON_PrintUnformatted(const cJSON *v) { return dup_string(cJSON_GetObjectItemCaseSensitive(v,"type")->valuestring); }
static cJSON *message(const char *type) { cJSON *v=cJSON_CreateObject(); cJSON_AddStringToObject(v,"type",type); cJSON_AddNumberToObject(v,"version",1); return v; }
static void format(cJSON *v) { cJSON *f=field(v,"format"); cJSON_AddNumberToObject(f,"sampleRate",48000); cJSON_AddNumberToObject(f,"channels",1); cJSON_AddNumberToObject(f,"bitsPerSample",16); cJSON_AddNumberToObject(f,"samplesPerFrame",960); }
static void dispatch_fixture(void) {
    esp_websocket_event_data_t e={.data_ptr="fixture",.data_len=7,.payload_len=7,.op_code=1,.fin=true};
    in_callback=true; websocket_event_handler(NULL,NULL,WEBSOCKET_EVENT_DATA,&e); in_callback=false;
}
static void prepare(void) {
    fixture=message("prepared"); format(fixture);
    cJSON_AddStringToObject(fixture,"requestID",s_request_id);
    cJSON_AddStringToObject(fixture,"sessionID","AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE");
    cJSON_AddNumberToObject(fixture,"generation",1); dispatch_fixture();
}
static void deliver(const char *type,uint32_t seq) {
    fixture=message(type); cJSON_AddStringToObject(fixture,"sessionID",s_session_id);
    cJSON_AddNumberToObject(fixture,"generation",(double)s_generation);
    cJSON_AddNumberToObject(fixture,"sequence",seq); cJSON_AddNumberToObject(fixture,"finalSequence",seq);
    dispatch_fixture();
}
void *heap_caps_malloc(size_t n,unsigned caps) { assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)); assert(n==48000); return fail_allocation?NULL:malloc(n); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { assert(lock_count<8); locks[lock_count]=(struct fake_semaphore){1,true}; return &locks[lock_count++]; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { SemaphoreHandle_t s=xSemaphoreCreateMutex(); s->available=0; s->mutex=false; return s; }
int xSemaphoreTake(SemaphoreHandle_t s,TickType_t timeout) {
    if(run_stream && binary_sends>=stream_send_limit && s==s_state_lock && s_send_lock->available && atomic_load(&s_stream_ticks)>send_tick) longjmp(stream_exit,1);
    if(s==s_power_lock && before_power){void(*f)(void)=before_power;before_power=NULL;f();}
    if(!s->available && timeout && !s->mutex) {
        assert(s_queue_lock->available);
        if(check_mutexes) assert(s_state_lock->available && s_send_lock->available);
        time_us+=(int64_t)timeout*1000;
        if(run_stream) check_liveness();
    }
    if(!s->available)return pdFALSE;
    s->available=0; return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t s) { assert(!s->mutex || !s->available); s->available=1; return pdTRUE; }
QueueHandle_t xQueueCreateStatic(unsigned n,size_t width,uint8_t *storage,StaticQueue_t *control) { assert(n==25&&width==1920&&storage&&control); queue_store=(struct fake_queue){n,0,0,width,storage}; return &queue_store; }
int xQueueReset(QueueHandle_t q) { q->count=q->head=0; if(during_reset){void(*f)(void)=during_reset;during_reset=NULL;f();} return pdTRUE; }
int xQueueSend(QueueHandle_t q,const void *data,TickType_t t) { assert(t==0); if(q->count==q->capacity)return pdFALSE; memcpy(q->data+((q->head+q->count)%q->capacity)*q->width,data,q->width); ++q->count;return pdTRUE; }
int xQueueReceive(QueueHandle_t q,void *data,TickType_t t) { assert(t==0); if(!q->count)return pdFALSE; memcpy(data,q->data+q->head*q->width,q->width); q->head=(q->head+1)%q->capacity; --q->count;return pdTRUE; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(time_us/1000); }
int64_t esp_timer_get_time(void) { return time_us; }
void vTaskDelay(TickType_t t) { time_us+=(int64_t)t*1000; if(run_stream)longjmp(stream_exit,1); longjmp(capture_exit,1); }
int xTaskCreate(void(*f)(void*),const char *name,unsigned stack,void *a,unsigned p,TaskHandle_t *h) { (void)f;(void)stack;(void)a;(void)p; if(!strcmp(name,"voice_capture")&&fail_capture_task)return pdFALSE; if(h)*h=(void*)1; return pdPASS; }
esp_err_t bsp_audio_init(const i2s_std_config_t *c) { (void)c; return ESP_OK; }
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void) { return (void*)1; }
int esp_codec_dev_open(esp_codec_dev_handle_t d,const esp_codec_dev_sample_info_t *f) { (void)d;(void)f;return ESP_CODEC_DEV_OK; }
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t d,float g) { (void)d;(void)g;return ESP_CODEC_DEV_OK; }
int esp_codec_dev_read(esp_codec_dev_handle_t d,void *out,int size) {
    (void)d; if(codec_reads++)longjmp(capture_exit,1);
    memset(out,0x35,(size_t)size); time_us+=20000;
    if(during_read){void(*f)(void)=during_read;during_read=NULL;f();}
    return no_codec?ESP_FAIL:ESP_CODEC_DEV_OK;
}
static void capture_one(void) { codec_reads=0; if(setjmp(capture_exit)==0)capture_task(NULL); s_codec_lock->available=1; }
static void stream_one(void) { run_stream=true;stream_send_limit=binary_sends+1; if(setjmp(stream_exit)==0)stream_task(NULL);run_stream=false; }
EventGroupHandle_t xEventGroupCreate(void) { event_bits=0;return &event_bits; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t e,EventBits_t b) { return *e|=b; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t e,EventBits_t b) { EventBits_t old=*e; *e&=~b;return old; }
EventBits_t xEventGroupGetBits(EventGroupHandle_t e) { return *e; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t e,EventBits_t bits,int clear,int all,TickType_t timeout) {
    (void)clear;(void)all;
    if(during_wait){void(*f)(void)=during_wait;during_wait=NULL;f();}
    if(!(*e&bits)&&handshake&&(bits&WIRELESS_EVENT_LISTENING)) {
        service_connection_once();
        if(s_phase==SESSION_STREAMING&&!no_codec) {capture_one();stream_one();}
    }
    if(!(*e&bits))time_us+=(int64_t)timeout*1000;
    return *e;
}
bool wifi_manager_wait_connected(uint32_t t) { (void)t;return true; }
bool wifi_manager_is_connected(void) { return true; }
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *out) { *out=ps;return ESP_OK; }
esp_err_t esp_wifi_set_ps(wifi_ps_type_t v) { assert(s_state_lock==NULL||s_state_lock->available);ps=v;return ESP_OK; }
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *c) { assert(c->enable_close_reconnect);return (void*)1; }
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t c,int id,void(*f)(void*,esp_event_base_t,int32_t,void*),void *a) { (void)c;(void)id;(void)f;(void)a;return ESP_OK; }
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t c) { (void)c;assert(!in_callback);++start_calls;return ESP_OK; }
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t c) { (void)c;assert(!in_callback);++stop_calls;websocket_event_handler(NULL,NULL,WEBSOCKET_EVENT_DISCONNECTED,NULL);return ESP_OK; }
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t c) { (void)c;return true; }
int esp_websocket_client_send_text(esp_websocket_client_handle_t c,const char *data,int n,TickType_t t) {
    (void)c;(void)t; assert(!in_callback);assert(s_state_lock->available);
    if(!strcmp(data,"hello"))++hello_sends;
    if(!strcmp(data,"cancel"))++cancel_sends;
    if(!strcmp(data,"start")){assert(ps==WIFI_PS_NONE);if(handshake)prepare();}
    if(!strcmp(data,"commit")&&!omit_armed) {
        if(fail_gate)s_queue_lock->available=0;
        if(revoke_before_arm)(void)voice_audio_revoke_capture();
        deliver("armed",0);s_queue_lock->available=1;
    }
    if(!strcmp(data,"stop")&&stop_ack)deliver("stopped",s_next_sequence);
    return n;
}
int esp_websocket_client_send_bin(esp_websocket_client_handle_t c,const char *data,int n,TickType_t t) {
    (void)c;(void)t; assert(!in_callback&&s_state_lock->available);
    wireless_microphone_audio_frame_view_t frame;
    assert(wireless_microphone_decode_audio_frame((const uint8_t*)data,(size_t)n,&frame));
    assert(frame.sequence==s_next_sequence&&frame.first_sample==(uint64_t)frame.sequence*960);
    assert(frame.pcm_length==1920&&frame.pcm[0]==0x35);
    ++binary_sends; send_tick=atomic_load(&s_stream_ticks);
    if(fail_binary) {time_us+=110000;in_callback=true;websocket_event_handler(NULL,NULL,WEBSOCKET_EVENT_ERROR,NULL);in_callback=false;errno=0;return 0;}
    deliver(frame.sequence==0?"listening":"ack",frame.sequence);
    return n;
}

static void setup(void) {
    free(s_capture_storage);s_capture_storage=NULL;free(s_certificate_pem);s_certificate_pem=NULL;
    s_state_lock=s_send_lock=s_power_lock=NULL;lock_count=0;time_us=100000;
    fail_allocation=fail_capture_task=fail_binary=handshake=stop_ack=omit_armed=fail_gate=revoke_before_arm=no_codec=run_stream=in_callback=false;
    check_mutexes=true; during_read=during_reset=during_wait=before_power=NULL;
    binary_sends=stop_calls=start_calls=cancel_sends=hello_sends=0; ps=WIFI_PS_MAX_MODEM;
    s_phase=SESSION_IDLE;s_attempt=0;s_capture_token=0;s_start_waiter=s_stop_waiter=s_send_in_flight=false;
    s_recovery_requested=s_recovering=s_failure_notice=s_wifi_power_save_saved=false;
    s_hello_pending=s_commit_pending=false;s_generation=s_next_sequence=0;s_last_session_failure[0]=s_session_id[0]=0;
    assert(voice_audio_init()==ESP_OK);assert(wireless_microphone_init()==ESP_OK);
    s_connected=s_authenticated=true;s_transport_poisoned=false;reset_rx();
}
static esp_err_t start_take(void) { uint32_t token=voice_audio_revoke_capture(); return wireless_microphone_start_session_authorized("task","request",token); }
static void reopen(void) { voice_audio_set_listening(false);assert(voice_audio_start_capture(VOICE_AUDIO_SOURCE_WIFI,voice_audio_capture_token())==ESP_OK); }
static void revoke(void) { (void)voice_audio_revoke_capture(); }
static void cancel_wait(void) { assert(wireless_microphone_stop_session()==ESP_OK); }
static void cancel_power(void) { cancel_preparation(s_attempt); }
static void activate_audio(void) { assert(voice_audio_start_capture(VOICE_AUDIO_SOURCE_WIFI,voice_audio_capture_token())==ESP_OK); }
static void consume_notice(void) { assert(wireless_microphone_take_failure());assert(s_phase==SESSION_FAILED);assert(event_bits&WIRELESS_EVENT_FAILED); }
static void healthy(void) { handshake=true;assert(start_take()==ESP_OK);assert(s_phase==SESSION_STREAMING&&binary_sends==1&&s_next_sequence==1); }

static void audio_tests(void) {
    setup();assert(voice_audio_is_ready());const uint32_t token=voice_audio_revoke_capture();revoke();
    assert(voice_audio_start_capture(VOICE_AUDIO_SOURCE_WIFI,token)==ESP_ERR_INVALID_STATE);assert(!voice_audio_is_listening());
    puts("PASS stop before activation entry invalidates authorization");
    setup();during_reset=revoke;assert(voice_audio_start_capture(VOICE_AUDIO_SOURCE_WIFI,voice_audio_capture_token())==ESP_ERR_INVALID_STATE);assert(!voice_audio_is_listening());
    puts("PASS stop during reset wins opening CAS");
    setup();const uint32_t old=voice_audio_revoke_capture();assert(voice_audio_start_capture(VOICE_AUDIO_SOURCE_WIFI,old)==ESP_OK);
    const uint32_t next=voice_audio_revoke_capture();assert(voice_audio_start_capture(VOICE_AUDIO_SOURCE_WIFI,next)==ESP_OK);voice_audio_stop_capture(old);assert(voice_audio_is_listening());
    puts("PASS old cleanup cannot mute successor");
    setup();s_queue_lock->available=0;assert(voice_audio_start_capture(VOICE_AUDIO_SOURCE_WIFI,0)==ESP_ERR_TIMEOUT);assert(!voice_audio_is_listening());s_queue_lock->available=1;
    puts("PASS capture lock timeout is explicit and gate stays closed");
    setup();activate_audio();during_read=reopen;capture_one();assert(queue_store.count==0);capture_one();assert(queue_store.count==1);
    puts("PASS codec read spanning stop/start is discarded");
    setup();during_read=activate_audio;capture_one();assert(queue_store.count==0);capture_one();assert(queue_store.count==1);
    puts("PASS pre-arm codec read discarded and fresh frame retained");
    uint8_t pcm[1920];size_t bytes;
    setup();activate_audio();capture_one();voice_audio_set_host_muted(true);assert(voice_audio_is_listening());
    assert(voice_audio_read(pcm,sizeof(pcm),&bytes)==ESP_OK);for(size_t i=0;i<sizeof(pcm);++i)assert(pcm[i]==0);
    assert(queue_store.count==1);assert(voice_audio_wireless_read_frame(pcm,sizeof(pcm),&bytes)==ESP_OK&&bytes==1920&&pcm[0]==0x35);
    puts("PASS USB polls/mute cannot consume or suppress Wi-Fi capture");
    setup();activate_audio();for(int i=0;i<25;++i)capture_one();assert(queue_store.count==25&&!voice_audio_take_overflow());capture_one();assert(queue_store.count==25&&voice_audio_take_overflow());assert(!voice_audio_take_overflow());
    puts("PASS 500 ms PSRAM ring bounded and overflow latched");
    setup();activate_audio();assert(voice_audio_wireless_read_frame(pcm,sizeof(pcm),&bytes)==ESP_ERR_TIMEOUT&&bytes==0);
    puts("PASS missing frame waits without producer/state/send mutex");
    setup();activate_audio();no_codec=true;capture_one();voice_audio_diagnostics_t d;voice_audio_get_diagnostics(&d);assert(d.read_errors>0);
    puts("PASS codec failure counter is independent of queue progress");
    setup();atomic_store(&s_ready,false);assert(!wireless_microphone_is_ready());assert(start_take()==ESP_ERR_INVALID_STATE);
    puts("PASS paired connection does not imply capture readiness");
}
static void wireless_tests(void) {
    setup();healthy();assert(voice_audio_is_listening());stop_ack=true;assert(wireless_microphone_stop_session()==ESP_OK);assert(!voice_audio_is_listening()&&s_phase==SESSION_IDLE&&ps==WIFI_PS_MAX_MODEM);deliver("armed",0);assert(!voice_audio_is_listening());
    puts("PASS actual armed -> actual capture -> frame -> listening -> drained stop");
    setup();healthy();deliver("armed",0);assert(s_next_sequence==1&&voice_audio_is_listening());
    puts("PASS duplicate armed cannot reset sequence or capture");
    setup();handshake=true;fail_gate=true;assert(start_take()==ESP_FAIL);assert(s_phase==SESSION_FAILED&&!voice_audio_is_listening()&&binary_sends==0);assert(strstr(s_last_session_failure,"gate lock"));consume_notice();assert(!wireless_microphone_take_failure());service_connection_once();assert(ps==WIFI_PS_MAX_MODEM&&stop_calls==1);
    puts("PASS real gate failure terminates start; UI notice preserves terminal result");
    setup();handshake=true;revoke_before_arm=true;assert(start_take()==ESP_ERR_INVALID_STATE);assert(s_phase==SESSION_CANCELED&&!voice_audio_is_listening()&&binary_sends==0);service_connection_once();assert(ps==WIFI_PS_MAX_MODEM);
    puts("PASS stop before armed handler cannot reopen gate");
    setup();handshake=true;omit_armed=true;assert(start_take()==ESP_ERR_TIMEOUT);assert(atomic_load(&s_armed_received)==0&&binary_sends==0);service_connection_once();assert(stop_calls==1);
    puts("PASS absent armed distinguished from activation failure");
    setup();handshake=true;no_codec=true;assert(start_take()==ESP_ERR_TIMEOUT);assert(atomic_load(&s_armed_received)==1&&atomic_load(&s_send_attempts)==0);service_connection_once();assert(!voice_audio_is_listening()&&ps==WIFI_PS_MAX_MODEM);
    puts("PASS startup timeout snapshot distinguishes gate-open/no-frame");
    setup();during_wait=cancel_wait;assert(start_take()==ESP_ERR_INVALID_STATE);assert(s_phase==SESSION_CANCELED&&s_generation==0);service_connection_once();assert(stop_calls==1&&ps==WIFI_PS_MAX_MODEM&&!s_recovery_requested);
    s_connected=s_authenticated=true;healthy();
    puts("PASS cancel before prepared retires connection and allows new gesture");
    setup();before_power=cancel_power;assert(start_take()==ESP_ERR_INVALID_STATE);assert(ps==WIFI_PS_MAX_MODEM);
    puts("PASS canceled preparation cannot acquire recording power late");
    setup();handshake=true;fail_binary=true;assert(start_take()==ESP_FAIL);assert(!voice_audio_is_listening()&&!s_send_in_flight);assert(strstr(s_last_session_failure,"audio send r=0"));assert(cancel_sends==0&&stop_calls==0);service_connection_once();assert(stop_calls==1&&start_calls==1&&cancel_sends==0&&ps==WIFI_PS_MAX_MODEM);
    puts("PASS synchronous send-error callback cannot deadlock/reenter transmitter");
    setup();healthy();voice_audio_set_listening(false);time_us+=1100000;check_liveness();assert(s_phase==SESSION_FAILED);service_connection_once();assert(ps==WIFI_PS_MAX_MODEM);
    puts("PASS closed gate cannot bypass independent stream watchdog");
    setup();healthy();time_us+=1100000;check_liveness();assert(s_phase==SESSION_FAILED&&!voice_audio_is_listening());
    puts("PASS starved sender cannot bypass independent progress deadline");
    setup();healthy();deliver("ack",0);const int64_t ack=s_last_ack_ms;time_us+=100000;deliver("ack",0);assert(s_last_ack_ms==ack);deliver("ack",5);assert(s_phase==SESSION_FAILED);
    puts("PASS only forward ACK progress counts; unsent ACK rejected");
    setup();healthy();assert(wireless_microphone_stop_session()==ESP_ERR_TIMEOUT);assert(s_phase==SESSION_FAILED&&!voice_audio_is_listening());service_connection_once();assert(ps==WIFI_PS_MAX_MODEM);
    puts("PASS stop acknowledgement loss retires take without replay");
    setup();s_phase=SESSION_WAIT_ARMED;s_attempt=1;s_capture_token=voice_audio_capture_token();s_generation=1;strcpy(s_session_id,"AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE");
    fixture=message("armed");cJSON_AddStringToObject(fixture,"sessionID",s_session_id);cJSON_AddNumberToObject(fixture,"generation",1);
    esp_websocket_event_data_t part={.op_code=1,.fin=true,.data_ptr="fix",.data_len=3,.payload_len=7};
    websocket_event_handler(NULL,NULL,WEBSOCKET_EVENT_DATA,&part);assert(s_phase==SESSION_WAIT_ARMED);
    esp_websocket_event_data_t ping={.op_code=9,.fin=true};websocket_event_handler(NULL,NULL,WEBSOCKET_EVENT_DATA,&ping);
    part.data_ptr="ture";part.data_len=4;part.payload_offset=3;websocket_event_handler(NULL,NULL,WEBSOCKET_EVENT_DATA,&part);assert(s_phase==SESSION_STREAMING);
    puts("PASS segmented control DATA with interleaved ping assembles once");
    setup();part=(esp_websocket_event_data_t){.op_code=1,.fin=true,.data_ptr="x",.data_len=1,.payload_len=4097};assert(!receive_control_chunk(&part));
    puts("PASS oversized control rejected before copy");
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"audio"))audio_tests();else wireless_tests();
    free(s_capture_storage);free(s_certificate_pem);
    return 0;
}

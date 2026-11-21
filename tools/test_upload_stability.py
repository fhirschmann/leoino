"""Run production upload callbacks and worker on host threads with fault injection.

Uses ASan/UBSan; mocks only HTTP requests, files and FreeRTOS/Arduino APIs.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/WebExplorer.cpp').read_text()

def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'

prelude = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using String=std::string;using BaseType_t=int;
constexpr int pdPASS=1,pdTRUE=1,portPRIVILEGE_BIT=0,portTICK_PERIOD_MS=1;
constexpr int MALLOC_CAP_DEFAULT=0,MALLOC_CAP_INTERNAL=0;
constexpr int LOGLEVEL_ERROR=0,LOGLEVEL_INFO=1,LOGLEVEL_DEBUG=2;
const char *unableToAllocateMem="oom",*writingFile="write",*webTxCanceled="cancel",*fileWritten="done";
void Log_Println(const char*,int){}template<class... T> void Log_Printf(T...){}
void System_UpdateActivityTimer(){}void System_PauseTasksDuringUpload(bool){}
uint32_t millis(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void vTaskDelay(int n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}
int pdMS_TO_TICKS(int n){return n;}void vTaskDelete(void*){}
struct Semaphore{std::mutex m;std::condition_variable cv;bool ready=false;};using SemaphoreHandle_t=Semaphore*;
Semaphore semaphore;bool failSemaphore=false,failPath=false,failTask=false,failAlloc=false;
Semaphore* xSemaphoreCreateBinary(){return failSemaphore?nullptr:&semaphore;}
void xSemaphoreGive(Semaphore* s){std::lock_guard<std::mutex> l(s->m);s->ready=true;s->cv.notify_all();}
int xSemaphoreTake(Semaphore* s,int n){std::unique_lock<std::mutex> l(s->m);if(!s->cv.wait_for(l,std::chrono::milliseconds(n),[&]{return s->ready;}))return 0;s->ready=false;return 1;}
size_t allocLimit=SIZE_MAX;
void* heap_caps_aligned_alloc(size_t a,size_t n,int){return failAlloc||n>allocLimit?nullptr:aligned_alloc(a,n);}
char* x_strdup(const char* s){return failPath?nullptr:strdup(s);}
std::thread worker;
int xTaskCreatePinnedToCore(void(*fn)(void*),const char*,int,void* arg,int,void*,int){if(failTask)return 0;if(worker.joinable())worker.join();worker=std::thread(fn,arg);return 1;}
struct AsyncWebParameter{String value() const{return "";}};
struct AsyncWebServerRequest {
 void* _tempObject=nullptr;bool authorized=true;int status=0;std::function<void()> disconnected;
 ~AsyncWebServerRequest(){if(_tempObject)delete static_cast<int*>(_tempObject);}
 bool hasParam(const char*){return false;}const AsyncWebParameter* getParam(const char*){return nullptr;}
 void send(int n){status=n;}void onDisconnect(std::function<void()> f){disconnected=f;}
};
bool Web_RequestAuthorized(AsyncWebServerRequest* r){return r->authorized;}
std::map<std::string,std::vector<uint8_t>> files;
std::atomic<bool> shortWrites=false,failOpen=false;
struct File{
 std::vector<uint8_t>* data=nullptr;
 explicit operator bool()const{return data;}
 void setBufferSize(size_t){}void close(){}void flush(){}
 size_t write(const uint8_t* p,size_t n){if(shortWrites&&n>1)n=(n+1)/2;data->insert(data->end(),p,p+n);return n;}
};
struct FS{File open(const char* path,const char*,bool){if(failOpen)return {};auto& f=files[path];f.clear();return {&f};}}gFSystem;
void explorerHandleFileStorageTask(void*);
'''
start = source.index('static const uint32_t start_chunk_size')
end = source.index('class PlaylistWriteGuard', start)
code = prelude + source[start:end]
for signature in ['void WebExplorer_AbortUpload(', 'void destroyDoubleBuffer(', 'void explorerHandleUploadComplete(',
                  'bool allocateDoubleBuffer(', 'void handleUploadError(', 'void explorerHandleFileUpload(',
                  'static void explorerStoreFile(', 'void explorerHandleFileStorageTask(']:
    code += function(signature)
code += r'''
void join(){if(worker.joinable())worker.join();}
void finish(AsyncWebServerRequest& r){join();explorerHandleUploadComplete(&r);if(r.disconnected)r.disconnected();}
int main(){
 std::vector<uint8_t> data(160003);for(size_t i=0;i<data.size();++i)data[i]=i%251;
 // A single callback larger than every ring buffer combined; repeated wrap-arounds.
 for(int n=0;n<20;++n){AsyncWebServerRequest r;explorerHandleFileUpload(&r,"large",0,data.data(),data.size(),true);finish(r);assert(r.status==200);assert(files["/large"]==data);}
 // Fallback buffers must not be reused with the original, larger capacity.
 {AsyncWebServerRequest r;allocLimit=4096;explorerHandleFileUpload(&r,"small",0,data.data(),100,true);assert(chunk_size==4096);
 allocLimit=SIZE_MAX;explorerHandleFileUpload(&r,"large-again",0,data.data(),data.size(),true);finish(r);assert(r.status==200&&files["/large-again"]==data);}
 {AsyncWebServerRequest r;explorerHandleUploadComplete(&r);assert(r.status==400);}
 // No parallel or unauthenticated request may alter the owner's transfer or free its buffers.
 {AsyncWebServerRequest a,b,unauthorized;unauthorized.authorized=false;
 explorerHandleFileUpload(&a,"owned",0,data.data(),10,false);
 auto* liveBuffer=buffer[0];
 explorerHandleFileUpload(&b,"other",0,data.data(),100,true);explorerHandleUploadComplete(&b);assert(b.status==409);assert(buffer[0]==liveBuffer);
 explorerHandleFileUpload(&unauthorized,"bad",0,data.data(),100,true);explorerHandleUploadComplete(&unauthorized);assert(unauthorized.status==401&&!uploadAborted);
 explorerHandleFileUpload(&a,"owned",10,data.data()+10,100,true);finish(a);assert(a.status==200&&files["/owned"].size()==110);}
 // Disconnect cannot release buffers while the worker may still be in write().
 {AsyncWebServerRequest r;explorerHandleFileUpload(&r,"abort",0,data.data(),10,false);auto* liveBuffer=buffer[0];r.disconnected();assert(buffer[0]==liveBuffer);join();assert(!uploadStorageActive);}
 // Allocation / task startup failures never enter a producer wait on a nonexistent worker.
 for(int fault=0;fault<4;++fault){destroyDoubleBuffer();AsyncWebServerRequest r;
 failAlloc=fault==0;failPath=fault==1;failTask=fault==2;failSemaphore=fault==3;explorerFileUploadFinished=nullptr;
 explorerHandleFileUpload(&r,"failure",0,data.data(),data.size(),true);finish(r);assert(r.status>=500&&!uploadStorageActive);
 failAlloc=failPath=failTask=failSemaphore=false;}
 // Partial SD writes must append only the unwritten tail, not duplicate the first bytes.
 {AsyncWebServerRequest r;shortWrites=true;explorerHandleFileUpload(&r,"partial",0,data.data(),3,true);finish(r);assert(r.status==200);assert(files["/partial"]==std::vector<uint8_t>(data.begin(),data.begin()+3));shortWrites=false;}
 {AsyncWebServerRequest r;failOpen=true;explorerHandleFileUpload(&r,"missing",0,data.data(),data.size(),true);finish(r);assert(r.status==500);failOpen=false;}
 destroyDoubleBuffer();puts("PASS upload: large callbacks, 20 ring wraps/stress runs, competing requests, disconnect, OOM/task failure, short writes, failed open");
}
'''
with tempfile.TemporaryDirectory(prefix='leoino-upload-') as temp:
    path = Path(temp) / 'upload.cpp'
    path.write_text(code)
    binary = Path(temp) / 'upload'
    subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-no-pie', '-pthread', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=90)

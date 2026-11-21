"""Run real firmware logging + pinned LogRingBuffer on host threads under ASan/UBSan.

Requires g++ and the dependencies fetched by the complete PlatformIO build.
Only Arduino/FreeRTOS platform calls are stubbed; ring and firmware code are copied unchanged.
"""
from pathlib import Path
import tempfile,subprocess,shutil
root=Path(__file__).resolve().parents[1]
t=Path(tempfile.mkdtemp(prefix='leoino-log-test-'))
(t/'Arduino.h').write_text(r'''
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
using boolean=bool;
using String=std::string;
using StaticSemaphore_t=std::mutex;
using SemaphoreHandle_t=std::mutex*;
constexpr int portMAX_DELAY=-1;
inline auto xSemaphoreCreateMutexStatic(StaticSemaphore_t* p){return p;}
inline void xSemaphoreTake(SemaphoreHandle_t p,int){p->lock();}
inline void xSemaphoreGive(SemaphoreHandle_t p){p->unlock();}
inline bool psramFound(){return false;}
inline void* ps_malloc(size_t n){return malloc(n);}
inline uint32_t millis(){return 1;}
class Print {public:
 virtual ~Print()=default;
 virtual size_t write(uint8_t)=0;
 size_t print(const char* s){size_t n=0;while(*s){n+=write(*s++);std::this_thread::yield();}return n;}
 size_t println(const char* s){return print(s)+print("\n");}
 int printf(const char* f,...){char b[512];va_list a;va_start(a,f);int n=vsnprintf(b,sizeof(b),f,a);va_end(a);print(b);return n;}
};
struct SerialMock {void begin(int){} template<class... T> void printf(T...){} void print(const char*){} void println(const char*){} };
inline SerialMock Serial;
''')
(t/'settings.h').write_text('#define SERIAL_LOGLEVEL 4\n')
(t/'logmessages.h').write_text('')
for name in ['Log.cpp','Log.h','MemX.h']:shutil.copy(root/'src'/name,t/name)
for name in ['LogRingBuffer.cpp','LogRingBuffer.h']:shutil.copy(root/'.pio/libdeps/complete/LogRingBuffer/src'/name,t/name)
(t/'main.cpp').write_text(r'''
#include "Arduino.h"
#include "Log.h"
#include <vector>
int main(){Log_Init();std::atomic<bool> done=false;
 std::thread reader([&]{while(!done){auto s=Log_GetRingBuffer();assert(s.size()<=2048);}});
 std::vector<std::thread> writers;
 for(int t=0;t<6;++t) writers.emplace_back([t]{for(int i=0;i<10000;++i)Log_Printf(LOGLEVEL_INFO,"volume %d, step %d",t,i);});
 for(auto& w:writers)w.join();done=true;reader.join();assert(Log_GetRingBuffer().size()==2048);
 puts("PASS: 60,000 concurrent log messages + snapshots, many buffer wraps");}
''')
subprocess.run(['g++','-std=c++17','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-pthread','-I'+str(t),str(t/'Log.cpp'),str(t/'LogRingBuffer.cpp'),str(t/'main.cpp'),'-o',str(t/'stress')],check=True)
subprocess.run([str(t/'stress')],check=True)
shutil.rmtree(t)

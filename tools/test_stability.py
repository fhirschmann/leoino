"""Host fault simulation of production functions, with ASan/UBSan.

The function bodies are read from src, never reimplemented in the test. Only file
I/O, clocks and the ESP32 transfer APIs are simulated. Requires g++.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = Path(os.environ.get('STABILITY_SOURCE', ROOT / 'src'))


def function(file, signature):
    source = (SRC / file).read_text()
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


PRELUDE = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
'''


def run(name, code, directory):
    path = directory / (name + '.cpp')
    path.write_text(PRELUDE + code)
    binary = directory / name
    subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined,float-cast-overflow',
                    '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-no-pie',
                    '-I' + str(directory), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


with tempfile.TemporaryDirectory(prefix='leoino-stability-') as temp:
    temp = Path(temp)
    (temp / 'Arduino.h').write_text('#pragma once\n#define PROGMEM\n')
    (temp / 'pgmspace.h').write_text('#pragma once\n#define PROGMEM\n')
    (temp / 'VolumeCurveLut.h').write_text((ROOT / 'src/VolumeCurveLut.h').read_text())
    run('volume', r'''
#include "VolumeCurveLut.h"
#define pgm_read_float(p) (*(p))
static std::atomic<uint8_t> gVolumeCurveType{0};
''' + function('AudioPlayer.cpp', 'void AudioPlayer_SetVolumeCurve(')
        + function('AudioPlayer.cpp', 'float Audio_GetVolume(') + r'''
int main() {
 for (int curve=0; curve<256; ++curve) {
  AudioPlayer_SetVolumeCurve(curve);
  float previous=Audio_GetVolume(0);
  for (int i=0;i<=10000;++i) {
   float value=Audio_GetVolume(float(i)/10000);
   assert(std::isfinite(value) && value>=previous && value>=-60 && value<=0);
   previous=value;
  }
  for(int n=0;n<1000;++n){assert(std::isfinite(Audio_GetVolume(1)));assert(std::isfinite(Audio_GetVolume(20.f/21)));}
  for(float t:{-INFINITY,-1.f,0.f,1.f,INFINITY,NAN,std::numeric_limits<float>::max()}) assert(std::isfinite(Audio_GetVolume(t)));
 }
 puts("PASS volume: all 256 settings, 2.56M ramp points, 256k maximum/back cycles, non-finite inputs");
}
''', temp)
    audio = (SRC / 'AudioPlayer.cpp').read_text()
    start = audio.index('// Use the actual delimiter:')
    end = audio.index('uint16_t currentTrack = 0;', start)
    metadata = 'const char* titleValue(const char* text){ struct {const char* msg;} m{text};\n' + audio[start:end] + 'return titleVal; }\n'
    run('metadata', metadata + r'''
int main(){
 for(const char* text:{"", "T", "Title", "Title/", "Title:", "Title: Hello", "TITLE=Hello", "Title/Songname/Content description: Hello"}){
  std::vector<char> exact(text,text+strlen(text)+1);
  const char* value=titleValue(exact.data());
  if(strstr(text,"Hello"))assert(!strcmp(value,"Hello"));else assert(!*value);
 }
 puts("PASS metadata: truncated ID3 labels and all supported title delimiters");
}
''', temp)
    common = (ROOT / 'src/Common.cpp').read_text()
    decoder = common[common.index('static const int B64index'):common.index('String Url_EncodePath')]
    run('cover', r'''
struct File {
 std::vector<uint8_t> data; size_t pos=0; bool shortRead=false,shortWrite=false,failSeek=false;
 size_t size() const{return data.size();}
 bool seek(size_t n){if(failSeek||n>data.size())return false;pos=n;return true;}
 size_t readBytes(char* out,size_t n){n=std::min(n,data.size()-pos);if(shortRead&&n)--n;memcpy(out,data.data()+pos,n);pos+=n;return n;}
 size_t write(const uint8_t* in,size_t n){if(shortWrite&&n)--n;data.insert(data.end(),in,in+n);return n;}
};
''' + decoder + function('AudioPlayer.cpp', 'static bool Audio_DecodeCoverSegments(') + r'''
int main(){
 const std::string base="QUJDREVG"; // ABCDEF
 std::string encoded; for(int i=0;i<1024;++i)encoded+=base;
 File source;source.data.assign(encoded.begin(),encoded.end());uint8_t buffer[2048];
 for(size_t split=0;split<=encoded.size();++split){
  File out;std::vector<uint32_t> parts={0,uint32_t(split),uint32_t(split),uint32_t(encoded.size()-split)};
  assert(Audio_DecodeCoverSegments(source,out,parts,buffer,sizeof(buffer)));
  std::string expected="fLaC";for(int i=0;i<1024;++i)expected+="ABCDEF";
  assert(std::string(out.data.begin(),out.data.end())==expected);
 }
 for(auto parts:std::vector<std::vector<uint32_t>>{{},{0},{0,2,4},{UINT32_MAX,2},{0,UINT32_MAX},{8192,1},{0,1}}){
  File out;assert(!Audio_DecodeCoverSegments(source,out,parts,buffer,sizeof(buffer)));
 }
 for(int fault=0;fault<3;++fault){File out;source.shortRead=fault==0;source.failSeek=fault==1;out.shortWrite=fault==2;
  assert(!Audio_DecodeCoverSegments(source,out,{0,8192},buffer,sizeof(buffer)));}
 puts("PASS cover: 8,193 segment splits, exact decoded bytes, invalid vectors, seek/read/write faults");
}
''', temp)
    # Real driver class and transfer methods; minimal IDF declarations for the unused init methods.
    (temp / 'driver').mkdir()
    (temp / 'driver/rmt_tx.h').write_text('''#pragma once
#include <cstddef>
using rmt_channel_handle_t=void*; using rmt_encoder_handle_t=void*;
struct rmt_transmit_config_t {};
''')
    (temp / 'driver/spi_master.h').write_text('''#pragma once
#include <cstddef>
using spi_device_handle_t=void*;
struct spi_transaction_t {size_t length=0; const void* tx_buffer=nullptr;};
''')
    (temp / 'LedDriver.h').write_text((SRC / 'LedDriver.h').read_text())
    driver = (SRC / 'LedDriver.cpp').read_text()
    nibble = driver[driver.index('struct SpiNibbleTable'):driver.index('static inline uint8_t scale8Video')]
    wait_signature = 'bool LedDriver::waitDone(' if 'bool LedDriver::waitDone(' in driver else 'void LedDriver::waitDone('
    run('led', r'''
#define private public
#include "LedDriver.h"
#undef private
using esp_err_t=int;constexpr int ESP_OK=0,transferTimeout=100,refreshIntervalMs=1000;
uint32_t now=10;uint32_t millis(){return now;}uint32_t micros(){return now*1000;}
int pdMS_TO_TICKS(int n){return n;}void vTaskDelay(int){}
int waitResult=0,queueResult=0,queued=0;std::vector<uint8_t> sent;
int spi_device_get_trans_result(void*,spi_transaction_t**,int){return waitResult;}
int rmt_tx_wait_all_done(void*,int){return waitResult;}
int spi_device_queue_trans(void*,spi_transaction_t* t,int){++queued;sent.assign((uint8_t*)t->tx_buffer,(uint8_t*)t->tx_buffer+t->length/8);return queueResult;}
int rmt_transmit(void*,void*,const void* b,size_t n,void*){++queued;sent.assign((uint8_t*)b,(uint8_t*)b+n);return queueResult;}
''' + nibble + function('LedDriver.cpp', wait_signature)
        + function('LedDriver.cpp', 'void LedDriver::transmit(') + r'''
int main(){for(auto backend:{LedDriver::Backend::Spi,LedDriver::Backend::Rmt}){
 LedDriver d;CRGB pixel(255,0,0);uint8_t frame[3]={},spi[12]={},rmt[3]={};
 d.mBackend=backend;d.mLeds=&pixel;d.mNumLeds=1;d.mFrame=frame;d.mSpiBuffer=spi;d.mSpiBufferLen=12;d.mRmtBuffer=rmt;
 queued=0;queueResult=0;waitResult=0;d.transmit(false);assert(queued==1);
 auto before=sent;pixel=CRGB(0,255,0);waitResult=1;
 for(int i=0;i<1000;++i){d.transmit(false);assert(queued==1);assert(d.mTransferPending);assert(!memcmp(backend==LedDriver::Backend::Spi?spi:rmt,before.data(),before.size()));}
 waitResult=0;d.transmit(false);assert(queued==2&&sent!=before);
 pixel=CRGB(0,0,255);queueResult=1;d.transmit(false);assert(queued==3);
 queueResult=0;d.transmit(false);assert(queued==4); // same frame must retry after failed queue
 d.transmit(false);assert(queued==4); // dedup after success
 }
 puts("PASS LED: SPI/RMT timeout preserves DMA buffer, pending transfer and retry; failed enqueue recovers");
}
''', temp)

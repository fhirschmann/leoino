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
    # Real WiFiSettings::serialize / deserialize under ASan/UBSan on empty, truncated, and malformed NVS blobs.
    wlan_h = (SRC / 'Wlan.h').read_text()
    wlan_struct = wlan_h[wlan_h.index('struct WiFiSettings {'):wlan_h.index('\nvoid Wlan_Init')]
    wlan_cpp = (SRC / 'Wlan.cpp').read_text()
    wlan_ser = wlan_cpp[wlan_cpp.index('std::optional<std::vector<uint8_t>> WiFiSettings::serialize()'):]
    run('wifi_deserialize', r'''
#include <functional>
#include <optional>
constexpr uint32_t INADDR_NONE = 0xFFFFFFFFu;
constexpr int LOGLEVEL_ERROR = 1;
void Log_Println(const char*, int) {}
struct IPAddress {
 uint32_t v{INADDR_NONE};
 IPAddress() = default;
 IPAddress(uint32_t x) : v(x) {}
 IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
  : v(uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24)) {}
 operator uint32_t() const { return v; }
 bool operator==(uint32_t o) const { return v == o; }
 bool operator!=(uint32_t o) const { return v != o; }
 bool operator==(const IPAddress &o) const { return v == o.v; }
};
struct String : std::string {
 using std::string::string;
 String(const char *p, size_t n) : std::string(p, n) {}
 bool isEmpty() const { return empty(); }
};
''' + wlan_struct + '\n' + wlan_ser + r'''
int main() {
 // Empty 0-byte NVS blob (used to dereference *nullptr on boot)
 { WiFiSettings empty(std::vector<uint8_t>{}); assert(!empty.isValid()); }
 // Valid round-trips (DHCP + Static IP) and every truncated prefix
 WiFiSettings dhcp; dhcp.ssid = "HomeWiFi"; dhcp.password = "supersecret123";
 auto dhcpBlob = dhcp.serialize(); assert(dhcpBlob.has_value());
 { WiFiSettings rt(*dhcpBlob); assert(rt.isValid() && rt.ssid == "HomeWiFi" && rt.password == "supersecret123" && !rt.staticIp.isValid()); }
 for (size_t cut = 0; cut < dhcpBlob->size(); ++cut) {
  std::vector<uint8_t> prefix(dhcpBlob->begin(), dhcpBlob->begin() + cut);
  WiFiSettings bad(prefix);
  assert(!bad.isValid());
 }
 WiFiSettings st; st.ssid = "LabNet"; st.password = "";
 st.staticIp = WiFiSettings::StaticIp(IPAddress(192,168,1,50), IPAddress(255,255,255,0), IPAddress(192,168,1,1), IPAddress(1,1,1,1), IPAddress(8,8,8,8));
 auto stBlob = st.serialize(); assert(stBlob.has_value());
 { WiFiSettings rt(*stBlob); assert(rt.isValid() && rt.staticIp.isValid() && rt.staticIp.addr == IPAddress(192,168,1,50) && rt.staticIp.dns2 == IPAddress(8,8,8,8)); }
 for (size_t cut = 0; cut < stBlob->size(); ++cut) {
  std::vector<uint8_t> prefix(stBlob->begin(), stBlob->begin() + cut);
  WiFiSettings bad(prefix);
  assert(!bad.isValid());
 }
 // Missing NUL, embedded NUL, invalid staticIp flag, trailing garbage
 { auto b = *dhcpBlob; b[1 + 8] = 'X'; WiFiSettings bad(b); assert(!bad.isValid()); }
 { auto b = *dhcpBlob; b[3] = '\0'; WiFiSettings bad(b); assert(!bad.isValid()); }
 { auto b = *dhcpBlob; b.back() = 2; WiFiSettings bad(b); assert(!bad.isValid()); }
 { auto b = *dhcpBlob; b.push_back(0xAA); WiFiSettings bad(b); assert(!bad.isValid()); }
 { std::vector<uint8_t> overLen = {255, 'a', 0}; WiFiSettings bad(overLen); assert(!bad.isValid()); }
 puts("PASS WiFiSettings: 0-byte blob, all truncated prefixes, missing/embedded NUL, bad flags, round-trip");
}
''', temp)
    # Real SafeJsonWebHandler under ASan/UBSan against chunked X-Expected-Entity-Length overflow lies + auth bypass.
    arduino_json_inc = ROOT / '.pio/libdeps/complete/ArduinoJson/src'
    if arduino_json_inc.is_dir():
        web_internal = (SRC / 'WebInternal.h').read_text()
        safe_cls = web_internal[web_internal.index('class SafeJsonWebHandler'):web_internal.index('// RFID tag-assignment handlers')]
        path = temp / 'safe_json.cpp'
        path.write_text(PRELUDE + r'''
#include <cstdlib>
#include <functional>
#include <ArduinoJson.h>
struct SpiRamAllocator : ArduinoJson::Allocator {
 void* allocate(size_t n) override { return malloc(n); }
 void deallocate(void* p) override { free(p); }
 void* reallocate(void* p, size_t n) override { return realloc(p, n); }
};
void* x_malloc(size_t n) { return malloc(n); }
void* x_realloc(void* p, size_t n) { return realloc(p, n); }
struct String : std::string {
 using std::string::string;
 bool equalsIgnoreCase(const char* o) const {
  if (size() != strlen(o)) return false;
  for (size_t i = 0; i < size(); ++i) if (tolower((*this)[i]) != tolower(o[i])) return false;
  return true;
 }
};
namespace asyncsrv { static constexpr const char* T_application_json = "application/json"; }
enum AsyncWebRequestMethod : uint8_t { HTTP_GET = 1, HTTP_POST = 2, HTTP_PUT = 4, HTTP_PATCH = 8 };
struct WebRequestMethodComposite {
 uint8_t m;
 WebRequestMethodComposite(uint8_t v) : m(v) {}
 bool matches(uint8_t v) const { return (m & v) != 0; }
};
struct AsyncWebServerRequest {
 void* _tempObject = nullptr;
 bool authorized = true;
 bool aborted = false;
 int status = 0;
 uint8_t _method = HTTP_POST;
 size_t _contentLen = 0;
 String _uri = "/settings";
 String _ct = "application/json";
 ~AsyncWebServerRequest() { free(_tempObject); }
 bool isHTTP() const { return true; }
 uint8_t method() const { return _method; }
 const String& contentType() const { return _ct; }
 size_t contentLength() const { return _contentLen; }
 void abort() { aborted = true; }
 void send(int c) { status = c; }
};
struct AsyncURIMatcher {
 std::string u;
 AsyncURIMatcher(const char* s) : u(s) {}
 bool matches(AsyncWebServerRequest* r) const { return r->_uri == u; }
};
class AsyncWebHandler {
public:
 virtual ~AsyncWebHandler() = default;
 virtual bool canHandle(AsyncWebServerRequest*) const { return false; }
 virtual void handleUpload(AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool) {}
 virtual void handleBody(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t) {}
 virtual void handleRequest(AsyncWebServerRequest*) {}
 virtual bool isRequestHandlerTrivial() const { return true; }
};
using ArJsonRequestHandlerFunction = std::function<void(AsyncWebServerRequest*, JsonVariant&)>;
bool Web_RequestAuthorized(AsyncWebServerRequest* r) { return r->authorized; }
''' + safe_cls + r'''
int main() {
 int seenVal = -1;
 SafeJsonWebHandler h("/settings", [&](AsyncWebServerRequest* r, JsonVariant& j) {
  seenVal = j["ok"] | -1;
  r->send(200);
 }, 1024, true);

 // 1) HTTP_GET is rejected by canHandle
 { AsyncWebServerRequest r; r._method = HTTP_GET; assert(!h.canHandle(&r)); }

 // 2) Unauthenticated request allocates zero body memory and returns 401
 {
  AsyncWebServerRequest r; r.authorized = false;
  std::string body = "{\"ok\":1}";
  h.handleBody(&r, (uint8_t*)body.data(), body.size(), 0, body.size());
  assert(r._tempObject == nullptr);
  h.handleRequest(&r);
  assert(r.status == 401 && seenVal == -1);
 }

 // 3) Lie: X-Expected-Entity-Length says total=4, chunked stream sends 600 bytes -> grows safely within cap!
 {
  AsyncWebServerRequest r;
  std::string body = "{\"ok\":42,\"pad\":\"" + std::string(550, 'x') + "\"}";
  h.handleBody(&r, (uint8_t*)body.data(), 200, 0, 4);
  h.handleBody(&r, (uint8_t*)body.data() + 200, body.size() - 200, 200, 4);
  h.handleRequest(&r);
  assert(r.status == 200 && seenVal == 42 && r._tempObject == nullptr);
 }

 // 4) Lie exceeding maxContentLength (1024): rejected cleanly with 413, zero heap overflow!
 {
  AsyncWebServerRequest r;
  std::vector<uint8_t> huge(2048, 'A');
  h.handleBody(&r, huge.data(), 500, 0, 4);
  h.handleBody(&r, huge.data(), 800, 500, 4);
  h.handleRequest(&r);
  assert(r.status == 413);
 }

 // 5) Standard chunked transfer without Content-Length (total=0 across multiple chunks)
 {
  AsyncWebServerRequest r; seenVal = -1;
  std::string body = "{\"ok\":99}";
  h.handleBody(&r, (uint8_t*)body.data(), 4, 0, 0);
  h.handleBody(&r, (uint8_t*)body.data() + 4, body.size() - 4, 4, 0);
  h.handleRequest(&r);
  assert(r.status == 200 && seenVal == 99 && r._tempObject == nullptr);
 }
 puts("PASS SafeJsonWebHandler: GET block, pre-body auth, X-Expected-Entity-Length overflow lie, 413 cap, chunked JSON");
}
''')
        binary = temp / 'safe_json'
        subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-no-pie',
                        '-I' + str(temp), '-I' + str(arduino_json_inc), str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

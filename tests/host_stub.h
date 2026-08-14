#ifndef HOST_STUB_H
#define HOST_STUB_H
#include <string>
#include <map>
#include <iostream>

using jstring = void*;
using jclass = void*;
using jlong = long long;
using jint = int;
using jboolean = unsigned char;
#define JNI_TRUE 1
#define JNI_FALSE 0

#define PROP_VALUE_MAX 92
int __system_property_get(const char* name, char* value) { return 0; }

struct JNIEnv {
    const char* GetStringUTFChars(jstring, void*) { return ""; }
    void ReleaseStringUTFChars(jstring, const char*) {}
    jstring NewStringUTF(const char*) { return nullptr; }
};

#define LOGD(...) do {} while(0)
#define LOGI(...) do {} while(0)
#define LOGE(...) do {} while(0)

#endif

#pragma once

#include <cstdint>
#include <cstdarg>

using jint     = int32_t;
using jlong    = int64_t;
using jsize    = jint;
using jboolean = uint8_t;
using jbyte    = int8_t;

#define JNI_FALSE 0
#define JNI_TRUE  1

struct _jobject {};
using jobject    = _jobject*;
using jclass     = jobject;
using jstring    = jobject;
using jthrowable = jobject;
using jarray     = jobject;
using jbyteArray = jobject;

struct _jmethodID {};
using jmethodID = _jmethodID*;
struct _jfieldID {};
using jfieldID = _jfieldID*;

struct JNINativeMethod {
    const char* name;
    const char* signature;
    void*       fnPtr;
};

struct JNIEnv {
    jint      PushLocalFrame(jint) { return 0; }
    jobject   PopLocalFrame(jobject) { return nullptr; }
    void      ExceptionClear() {}
    jboolean  ExceptionCheck() { return JNI_FALSE; }

    jclass    FindClass(const char*) { return nullptr; }
    jclass    GetObjectClass(jobject) { return nullptr; }
    jmethodID GetMethodID(jclass, const char*, const char*) { return nullptr; }
    jmethodID GetStaticMethodID(jclass, const char*, const char*) { return nullptr; }
    jfieldID  GetFieldID(jclass, const char*, const char*) { return nullptr; }
    jint      RegisterNatives(jclass, const JNINativeMethod*, jint) { return 0; }

    jobject   NewObject(jclass, jmethodID, ...) { return nullptr; }
    jobject   NewGlobalRef(jobject) { return nullptr; }
    jstring   NewStringUTF(const char*) { return nullptr; }
    const char* GetStringUTFChars(jstring, jboolean*) { return nullptr; }
    void      ReleaseStringUTFChars(jstring, const char*) {}
    jobject   NewDirectByteBuffer(void*, jlong) { return nullptr; }
    jbyteArray NewByteArray(jsize) { return nullptr; }
    void      SetByteArrayRegion(jbyteArray, jsize, jsize, const jbyte*) {}

    jobject   CallObjectMethod(jobject, jmethodID, ...) { return nullptr; }
    jobject   CallStaticObjectMethod(jclass, jmethodID, ...) { return nullptr; }

    jobject   ToReflectedMethod(jclass, jmethodID, jboolean) { return nullptr; }

    void SetBooleanField(jobject, jfieldID, jboolean) {}
    void SetIntField(jobject, jfieldID, jint) {}
    void SetObjectField(jobject, jfieldID, jobject) {}
};

#pragma once
// Minimal JNI stub — ONLY to host-syntax-check jni/sbx_lsplant.hpp under
// -DSBX_ENABLE_LSPLANT without an NDK. Not a functional JNI: signatures mirror
// the real <jni.h> closely enough that the L3 code type-checks. Never linked.
#include <cstdint>
#include <cstdarg>

using jint     = int32_t;
using jlong    = int64_t;
using jsize    = jint;
using jboolean = uint8_t;
using jbyte    = int8_t;

#define JNI_FALSE 0
#define JNI_TRUE  1

// Opaque reference types (distinct structs so overloads/params stay checkable).
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

struct JNIEnv {
    jint      PushLocalFrame(jint) { return 0; }
    jobject   PopLocalFrame(jobject) { return nullptr; }
    void      ExceptionClear() {}
    jboolean  ExceptionCheck() { return JNI_FALSE; }

    jclass    FindClass(const char*) { return nullptr; }
    jmethodID GetMethodID(jclass, const char*, const char*) { return nullptr; }
    jmethodID GetStaticMethodID(jclass, const char*, const char*) { return nullptr; }
    jfieldID  GetFieldID(jclass, const char*, const char*) { return nullptr; }

    jobject   NewObject(jclass, jmethodID, ...) { return nullptr; }
    jobject   NewGlobalRef(jobject) { return nullptr; }
    jstring   NewStringUTF(const char*) { return nullptr; }
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

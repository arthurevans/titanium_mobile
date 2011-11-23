/**
 * Appcelerator Titanium Mobile
 * Copyright (c) 2011 by Appcelerator, Inc. All Rights Reserved.
 * Licensed under the terms of the Apache Public License
 * Please see the LICENSE included with this distribution for details.
 */
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <v8.h>
#ifdef V8_DEBUGGER
# include <v8-debug.h>
#endif

#include "AndroidUtil.h"
#include "EventEmitter.h"
#include "JavaObject.h"
#include "JNIUtil.h"
#include "JSException.h"
#include "KrollBindings.h"
#include "ProxyFactory.h"
#include "ScriptsModule.h"
#include "TypeConverter.h"
#include "V8Util.h"

#include "V8Runtime.h"

#include "org_appcelerator_kroll_runtime_v8_V8Runtime.h"

#define TAG "V8Runtime"

namespace titanium {

Persistent<Context> V8Runtime::globalContext;
Persistent<Object> V8Runtime::krollGlobalObject;
Persistent<Array> V8Runtime::moduleContexts;

jobject V8Runtime::javaInstance;
bool V8Runtime::debuggerEnabled = false;

/* static */
void V8Runtime::collectWeakRef(Persistent<Value> ref, void *parameter)
{
	jobject v8Object = (jobject) parameter;
	ref.Dispose();
	JNIScope::getEnv()->DeleteGlobalRef(v8Object);
}

// Minimalistic logging function for internal JS
static Handle<Value> krollLog(const Arguments& args)
{
	HandleScope scope;
	uint32_t len = args.Length();

	if (len < 2) {
		return JSException::Error("log: missing required tag and message arguments");
	}

	Handle<String> tag = args[0]->ToString();
	Handle<String> message = args[1]->ToString();
	for (uint32_t i = 2; i < len; ++i) {
		message = String::Concat(String::Concat(message, String::New(" ")), args[i]->ToString());
	}

	String::Utf8Value tagValue(tag);
	String::Utf8Value messageValue(message);
	__android_log_print(ANDROID_LOG_DEBUG, *tagValue, *messageValue);

	return Undefined();
}

/* static */
void V8Runtime::bootstrap(Local<Object> global)
{
	EventEmitter::initTemplate();

	krollGlobalObject = Persistent<Object>::New(Object::New());
	moduleContexts = Persistent<Array>::New(Array::New());

	DEFINE_METHOD(krollGlobalObject, "log", krollLog);
	DEFINE_METHOD(krollGlobalObject, "binding", KrollBindings::getBinding);
	DEFINE_TEMPLATE(krollGlobalObject, "EventEmitter", EventEmitter::constructorTemplate);

	krollGlobalObject->Set(String::NewSymbol("runtime"), String::New("v8"));
	krollGlobalObject->Set(String::NewSymbol("moduleContexts"), moduleContexts);

	LOG_TIMER(TAG, "Executing kroll.js");

	TryCatch tryCatch;
	Handle<Value> result = V8Util::executeString(KrollBindings::getMainSource(), String::New("kroll.js"));

	if (tryCatch.HasCaught()) {
		V8Util::reportException(tryCatch, true);
	}
	if (!result->IsFunction()) {
		LOGF(TAG, "kroll.js result is not a function");
		V8Util::reportException(tryCatch, true);
	}

	Handle<Function> mainFunction = Handle<Function>::Cast(result);
	Local<Value> args[] = { Local<Value>::New(krollGlobalObject) };
	mainFunction->Call(global, 1, args);

	if (tryCatch.HasCaught()) {
		V8Util::reportException(tryCatch, true);
		LOGE(TAG, "Caught exception while bootstrapping Kroll");
	}
}

static void logV8Exception(Handle<Message> msg, Handle<Value> data)
{
	HandleScope scope;

	// Log reason and location of the error.
	LOGD(TAG, *String::Utf8Value(msg->Get()));
	LOGD(TAG, "%s @ %d >>> %s",
		*String::Utf8Value(msg->GetScriptResourceName()),
		msg->GetLineNumber(),
		*String::Utf8Value(msg->GetSourceLine()));
}

#ifdef V8_DEBUGGER
static jmethodID dispatchDebugMessage = NULL;

static void dispatchHandler()
{
	static JNIEnv *env = NULL;
	if (!env) {
		titanium::JNIUtil::javaVm->AttachCurrentThread(&env, NULL);
	}

	env->CallVoidMethod(V8Runtime::javaInstance, dispatchDebugMessage);
}
#endif

} // namespace titanium

#ifdef __cplusplus
extern "C" {
#endif

using namespace titanium;

/*
 * Class:     org_appcelerator_kroll_runtime_v8_V8Runtime
 * Method:    nativeInit
 * Signature: (Lorg/appcelerator/kroll/runtime/v8/V8Runtime;)J
 */
JNIEXPORT void JNICALL Java_org_appcelerator_kroll_runtime_v8_V8Runtime_nativeInit(JNIEnv *env, jobject self, jboolean useGlobalRefs, jboolean debuggerEnabled)
{
	HandleScope scope;
	titanium::JNIScope jniScope(env);

	// Log all uncaught V8 exceptions.
	V8::AddMessageListener(logV8Exception);
	V8::SetCaptureStackTraceForUncaughtExceptions(true);

	JavaObject::useGlobalRefs = useGlobalRefs;
	V8Runtime::debuggerEnabled = debuggerEnabled;

	V8Runtime::javaInstance = env->NewGlobalRef(self);
	JNIUtil::initCache();

	Persistent<Context> context = Persistent<Context>::New(Context::New());
	context->Enter();


#ifdef V8_DEBUGGER
	jclass v8RuntimeClass = env->FindClass("org/appcelerator/kroll/runtime/v8/V8Runtime");
	dispatchDebugMessage = env->GetMethodID(v8RuntimeClass, "dispatchDebugMessages", "()V");

	Debug::SetDebugMessageDispatchHandler(dispatchHandler);
	Debug::EnableAgent("titanium", 9999, true);
#endif

	V8Runtime::globalContext = context;
	V8Runtime::bootstrap(context->Global());

	LOG_HEAP_STATS(TAG);
}

static Persistent<Object> moduleObject;
static Persistent<Function> runModuleFunction;

/*
 * Class:     org_appcelerator_kroll_runtime_v8_V8Runtime
 * Method:    nativeRunModule
 * Signature: (Ljava/lang/String;Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_org_appcelerator_kroll_runtime_v8_V8Runtime_nativeRunModule
	(JNIEnv *env, jobject self, jstring source, jstring filename, jobject activityProxy)
{
	ENTER_V8(V8Runtime::globalContext);
	titanium::JNIScope jniScope(env);

	if (moduleObject.IsEmpty()) {
		moduleObject = Persistent<Object>::New(
			V8Runtime::krollGlobalObject->Get(String::New("Module"))->ToObject());

		runModuleFunction = Persistent<Function>::New(
			Handle<Function>::Cast(moduleObject->Get(String::New("runModule"))));
	}

	Handle<Value> jsSource = TypeConverter::javaStringToJsString(source);
	Handle<Value> jsFilename = TypeConverter::javaStringToJsString(filename);
	Handle<Value> jsActivity = TypeConverter::javaObjectToJsValue(activityProxy);

	Handle<Value> args[] = { jsSource, jsFilename, jsActivity };
	TryCatch tryCatch;

	runModuleFunction->Call(moduleObject, 3, args);

	if (tryCatch.HasCaught()) {
		V8Util::openJSErrorDialog(tryCatch);
		V8Util::reportException(tryCatch, true);
	}
}

JNIEXPORT void JNICALL Java_org_appcelerator_kroll_runtime_v8_V8Runtime_nativeProcessDebugMessages(JNIEnv *env, jobject self)
{
#ifdef V8_DEBUGGER
	v8::Debug::ProcessDebugMessages();
#endif
}

// This method disposes of all native resources used by V8 when
// all activities have been destroyed by the application.
//
// When a Persistent handle is Dispose()'d in V8, the internal
// pointer is not changed, handle->IsEmpty() returns false. 
// As a consequence, we have to explicitly reset the handle
// to an empty handle using Persistent<Type>()
//
// Since we use lazy initialization in a lot of our code,
// there's probably not an easier way (unless we use boolean flags)

JNIEXPORT void JNICALL Java_org_appcelerator_kroll_runtime_v8_V8Runtime_nativeDispose(JNIEnv *env, jobject runtime)
{
	JNIScope jniScope(env);

	// We use a new scope here so any new handles we create
	// while disposing are cleaned up before our global context
	// is disposed below.
	{
		HandleScope scope;

		// Any module that has been require()'d or opened via Window URL
		// will be cleaned up here. We setup the initial "moduleContexts"
		// Array and expose it on kroll above in nativeInit, and
		// module.js will insert module contexts into this array in
		// Module.prototype._runScript
		uint32_t length = V8Runtime::moduleContexts->Length();
		for (uint32_t i = 0; i < length; ++i) {
			Handle<Value> moduleContext = V8Runtime::moduleContexts->Get(i);

			// WrappedContext is simply a C++ wrapper for the V8 Context object,
			// and is used to expose the Context to javascript. See ScriptsModule for
			// implementation details
			WrappedContext *wrappedContext = NativeObject::Unwrap<WrappedContext>(moduleContext->ToObject());

			// Detach each context's global object, and dispose the context
			wrappedContext->GetV8Context()->DetachGlobal();
			wrappedContext->GetV8Context().Dispose();
		}

		// KrollBindings
		KrollBindings::dispose();
		EventEmitter::dispose();

		V8Runtime::moduleContexts.Dispose();
		V8Runtime::moduleContexts = Persistent<Array>();

		V8Runtime::globalContext->DetachGlobal();

	}

	// Dispose of each class' static cache / resources

	V8Util::dispose();
	ProxyFactory::dispose();

	moduleObject.Dispose();
	moduleObject = Persistent<Object>();

	runModuleFunction.Dispose();
	runModuleFunction = Persistent<Function>();

	V8Runtime::krollGlobalObject.Dispose();

	V8Runtime::globalContext->Exit();
	V8Runtime::globalContext.Dispose();

	// Removes the retained global reference to the V8Runtime 
	env->DeleteGlobalRef(V8Runtime::javaInstance);

	V8Runtime::javaInstance = NULL;
}

jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
	JNIUtil::javaVm = vm;
	return JNI_VERSION_1_4;
}

#ifdef __cplusplus
}
#endif

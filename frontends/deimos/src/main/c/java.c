/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "deimos.h"
#include "embedded.h"

#include <unistd.h>
#include <jni.h>

#ifdef CHARSET_EBCDIC
#ifdef OSD_POSIX
#include <ascii_ebcdic.h>
#define deimos_xlate_to_ascii(b) _e2a(b)
#define deimos_xlate_from_ascii(b) _a2e(b)
#endif
#else
#define deimos_xlate_to_ascii(b)  /* NOOP */
#define deimos_xlate_from_ascii(b)        /* NOOP */
#endif

static JavaVM *jvm = NULL;
static JNIEnv *env = NULL;

static jobject loader = NULL;

#define FALSE 0
#define TRUE !FALSE

static void shutdown(JNIEnv *env, jobject source, jboolean reload)
{
    log_debug("Shutdown requested (reload is %d)", reload);
    if (reload == TRUE)
        main_reload();
    else
        main_shutdown();
}

static void failed(JNIEnv *env, jobject source, jstring message)
{
    if (message) {
        const char *msg = (*env)->GetStringUTFChars(env, message, NULL);
        log_error("Failed %s", msg ? msg : "(null)");
        if (msg)
            (*env)->ReleaseStringUTFChars(env, message, msg);
    }
    else
        log_error("Failed requested");
    main_shutdown();
}

/* Automatically restart when the JVM crashes */
static void java_abort123(void)
{
    exit(123);
}

char *java_library(arg_data *args, home_data *data)
{
    char *libf = NULL;

    /* Did we find ANY virtual machine? */
    if (data->jnum == 0) {
        log_error("Cannot find any VM in Java Home %s", data->path);
        return NULL;
    }

    /* Select the VM */
    if (args->name == NULL) {
        libf = data->jvms[0]->libr;
        log_debug("Using default JVM in %s", libf);
    }
    else {
        for (int x = 0; x < data->jnum; x++) {
            if (data->jvms[x]->name == NULL)
                continue;
            if (strcmp(args->name, data->jvms[x]->name) == 0) {
                libf = data->jvms[x]->libr;
                log_debug("Using specific JVM in %s", libf);
                break;
            }
        }
        if (libf == NULL) {
            log_error("Invalid JVM name specified %s", args->name);
            return NULL;
        }
    }
    return libf;
}

typedef jint (*jvm_create_t)(JavaVM **, JNIEnv **, JavaVMInitArgs *);

/* Initialize the JVM and its environment, loading libraries and all */
bool java_init(arg_data *args, home_data *data)
{
#ifdef OS_DARWIN
    dso_handle apph = NULL;
    char appf[1024];
    struct stat sb;
#endif /* ifdef OS_DARWIN */
    jvm_create_t symb = NULL;
    JNINativeMethod nativemethods[2];
    JavaVMOption *opt = NULL;
    dso_handle libh   = NULL;
    jmethodID method = NULL;
    JavaVMInitArgs arg;
    char *libf = NULL;
    jint ret;
    int x;
    
    char shutdownmethod[] = "shutdown";
    char shutdownparams[] = "(Z)V";
    char failedmethod[]   = "failed";
    char failedparams[]   = "(Ljava/lang/String;)V";
    char daemonprocid[64];

    /* Decide WHAT virtual machine we need to use */
    libf = java_library(args, data);
    if (libf == NULL) {
        log_error("Cannot locate JVM library file");
        return false;
    }

    /* Initialize the DSO library */
    if (dso_init() != true) {
        log_error("Cannot initialize the dynamic library loader");
        return false;
    }

    /* Load the JVM library */
#if !defined(OSD_POSIX)
    libh = dso_link(libf);
    if (libh == NULL) {
        log_error("Cannot dynamically link to %s", libf);
        log_error("%s", dso_error());
        return false;
    }
    log_debug("JVM library %s loaded", libf);
#endif

#ifdef OS_DARWIN
    /*
       MacOS/X actually has two libraries, one with the REAL vm, and one for
       the VM startup.
       before JVM 1.4.1 The first one (libappshell.dyld) contains CreateVM
       JVM 1.4.1 through 1.5.* The library name is libjvm_compat.dylib
       starting with JVM 1.6 on OS X 10.6 the library name is libverify.dylib.
     */
    if (replace(appf, 1024, "$JAVA_HOME/../Libraries/libappshell.dylib",
                "$JAVA_HOME", data->path) != 0) {
        log_error("Cannot replace values in loader library");
        return false;
    }
    if (stat(appf, &sb)) {
        if (replace(appf, 1024, "$JAVA_HOME/../Libraries/libjvm_compat.dylib",
                    "$JAVA_HOME", data->path) != 0) {
            log_error("Cannot replace values in loader library");
            return false;
        }
    }
    if (stat(appf, &sb)) {
        if (replace(appf, 1024, "$JAVA_HOME/../Libraries/libverify.dylib",
                    "$JAVA_HOME", data->path) != 0) {
            log_error("Cannot replace values in loader library");
            return false;
        }
    }
    if (stat(appf, &sb)) {
        if (replace(appf, 1024, "$JAVA_HOME/../MacOS/libjli.dylib",
                    "$JAVA_HOME", data->path) != 0) {
            log_error("Cannot replace values in loader library");
            return false;
        }
    }
    apph = dso_link(appf);
    if (apph == NULL) {
        log_error("Cannot load required shell library %s", appf);
        return false;
    }
    log_debug("Shell library %s loaded", appf);
#endif /* ifdef OS_DARWIN */
#if defined(OSD_POSIX)
    /* BS2000 does not allow to call JNI_CreateJavaVM indirectly */
#else
    symb = (jvm_create_t)dso_symbol(libh, "JNI_CreateJavaVM");
    if (symb == NULL) {
#ifdef OS_DARWIN
        symb = (jvm_create_t)dso_symbol(apph, "JNI_CreateJavaVM");
        if (symb == NULL) {
#endif /* ifdef OS_DARWIN */
            log_error("Cannot find JVM library entry point");
            return false;
#ifdef OS_DARWIN
        }
#endif /* ifdef OS_DARWIN */
    }
    log_debug("JVM library entry point found (0x%08X)", symb);
#endif

    /* Prepare the VM initialization arguments */
    arg.version = JNI_VERSION_1_6;

#if defined(OSD_POSIX)
    if (JNI_GetDefaultJavaVMInitArgs(&arg) < 0) {
        log_error("Cannot init default JVM default args");
        return false;
    }
#endif
    arg.ignoreUnrecognized = FALSE;
    arg.nOptions = args->onum + 4; /* pid, ppid and abort */
    opt = (JavaVMOption *) malloc(arg.nOptions * sizeof(JavaVMOption));
    for (x = 0; x < args->onum; x++) {
        opt[x].optionString = strdup(args->opts[x]);
        deimos_xlate_to_ascii(opt[x].optionString);
        opt[x].extraInfo = NULL;
    }
    /* Add our daemon process id */
    snprintf(daemonprocid, sizeof(daemonprocid),
             "-Dcommons.daemon.process.id=%d", (int)getpid());
    opt[x].optionString = strdup(daemonprocid);
    deimos_xlate_to_ascii(opt[x].optionString);
    opt[x++].extraInfo  = NULL;
    snprintf(daemonprocid, sizeof(daemonprocid),
             "-Dcommons.daemon.process.parent=%d", (int)getppid());
    opt[x].optionString = strdup(daemonprocid);
    deimos_xlate_to_ascii(opt[x].optionString);
    opt[x++].extraInfo  = NULL;
    snprintf(daemonprocid, sizeof(daemonprocid),
             "-Dcommons.daemon.version=%s", DEIMOS_VERSION_STRING);
    opt[x].optionString = strdup(daemonprocid);
    deimos_xlate_to_ascii(opt[x].optionString);
    opt[x++].extraInfo  = NULL;
    opt[x].optionString = strdup("abort");
    deimos_xlate_to_ascii(opt[x].optionString);
    opt[x].extraInfo = (void *)java_abort123;
    arg.options = opt;

    /* Do some debugging */
    if (log_debug_flag == true) {
        log_debug("+-- DUMPING JAVA VM CREATION ARGUMENTS -----------------");
        log_debug("| Version:                       %#08x", arg.version);
        log_debug("| Ignore Unrecognized Arguments: %s",
                  arg.ignoreUnrecognized == TRUE ? "True" : "False");
        log_debug("| Extra options:                 %d", args->onum);

        for (x = 0; x < args->onum; x++) {
            deimos_xlate_from_ascii(opt[x].optionString);
            log_debug("|   \"%s\" (0x%08x)", opt[x].optionString,
                      opt[x].extraInfo);
            deimos_xlate_to_ascii(opt[x].optionString);
        }
        log_debug("+-------------------------------------------------------");
        log_debug("| Internal options:              %d", arg.nOptions - args->onum);

        for (; x < arg.nOptions; x++) {
            deimos_xlate_from_ascii(opt[x].optionString);
            log_debug("|   \"%s\" (0x%08x)", opt[x].optionString,
                      opt[x].extraInfo);
            deimos_xlate_to_ascii(opt[x].optionString);
        }
        log_debug("+-------------------------------------------------------");
    }

    /* And finally create the Java VM */
#if defined(OSD_POSIX)
    ret = JNI_CreateJavaVM(&jvm, &env, &arg);
#else
    ret = (*symb) (&jvm, &env, &arg);
#endif
    if (ret < 0) {
        log_error("Cannot create Java VM");
        return false;
    }
    log_debug("Java VM created successfully");

    deimos_xlate_to_ascii(shutdownmethod);
    nativemethods[0].name = shutdownmethod;
    deimos_xlate_to_ascii(shutdownparams);
    nativemethods[0].signature = shutdownparams;
    nativemethods[0].fnPtr = (void *)shutdown;
    deimos_xlate_to_ascii(failedmethod);
    nativemethods[1].name = failedmethod;
    deimos_xlate_to_ascii(failedparams);
    nativemethods[1].signature = failedparams;
    nativemethods[1].fnPtr = (void *)failed;
    
    // Load classloader class
    const jclass clazzloader = (*env)->DefineClass(
        env,
        "io/zatarox/satellite/impl/EmbeddedClassLoader",
        NULL,
        dump_get_content(EMBEDDEDCLASSLOADER_CLASS),
        dump_get_size(EMBEDDEDCLASSLOADER_CLASS)
    );

    // Prepare an array of bytes
    jbyteArray content = (*env)->NewByteArray(
        env,
        dump_get_size(SATELLITE_EMBEDDED_JAR)
    );

    // Inject jar content
    (*env)->SetByteArrayRegion(
        env,
        content,
        0,
        dump_get_size(SATELLITE_EMBEDDED_JAR),
        dump_get_content(SATELLITE_EMBEDDED_JAR)
    );
    
    // Create an instance of our internal classloader with embedded jar
    loader = (*env)->CallStaticObjectMethod(
        env,
        clazzloader,
        (*env)->GetStaticMethodID(
            env,
            clazzloader,
            "createBootstrap",
            "([B)Ljava/lang/Object;"
        ),
        content
    );
    
    if((*env)->ExceptionCheck(env)) {
        jthrowable ex = (*env)->ExceptionOccurred(env);
        jclass clazz = (*env)->GetObjectClass(env, ex);
        
        (*env)->CallVoidMethod(
            env,
            clazz,
            (*env)->GetMethodID(env, clazz, "printStackTrace", "()V")
        );

        (*env)->ExceptionClear(env);
        return false;
    } else {
        printf("----------------------------------------------------------------------------\n");
    }

    if ((*env)->RegisterNatives(env, (*env)->GetObjectClass(env, loader), nativemethods, 2) != 0) {
        log_error("Cannot register native methods");
        return false;
    }
    log_debug("Native methods registered");

    return true;
}

/* Destroy the Java VM */
bool JVM_destroy(int exit)
{
    jclass system = NULL;
    jmethodID method;
    char System[] = "java/lang/System";
    char exitclass[] = "exit";
    char exitparams[] = "(I)V";

    deimos_xlate_to_ascii(System);
    system = (*env)->FindClass(env, System);
    deimos_xlate_from_ascii(System);
    if (system == NULL) {
        log_error("Cannot find class %s", System);
        return false;
    }

    deimos_xlate_to_ascii(exitclass);
    deimos_xlate_to_ascii(exitparams);
    method = (*env)->GetStaticMethodID(env, system, exitclass, exitparams);
    if (method == NULL) {
        log_error("Cannot find \"System.exit(int)\" entry point");
        return false;
    }

    log_debug("Calling System.exit(%d)", exit);
    (*env)->CallVoidMethod(env, system, method, (jint) exit);

    /* We shouldn't get here, but just in case... */
    log_debug("Destroying the Java VM");
    if ((*jvm)->DestroyJavaVM(jvm) != 0)
        return false;
    log_debug("Java VM destroyed");
    return true;
}

/* Call the load method in our wrapper class */
bool java_load(arg_data *args)
{
    jclass stringClass       = NULL;
    jstring className        = NULL;
    jstring currentArgument  = NULL;
    jobjectArray stringArray = NULL;
    jmethodID method         = NULL;
    jboolean ret             = FALSE;
    int x;
    char lang[] = "java/lang/String";
    char load[] = "load";
    char loadparams[] = "(Ljava/lang/String;[Ljava/lang/String;)Z";

    deimos_xlate_to_ascii(args->jar);
    className = (*env)->NewStringUTF(env, args->jar);
    deimos_xlate_from_ascii(args->jar);

    deimos_xlate_to_ascii(lang);
    stringClass = (*env)->FindClass(env, lang);
    if (stringClass == NULL) {
        log_error("Cannot find class java/lang/String");
        return false;
    }

    stringArray = (*env)->NewObjectArray(env, args->anum, stringClass, NULL);
    if (stringArray == NULL) {
        log_error("Cannot create arguments array");
        return false;
    }

    for (x = 0; x < args->anum; x++) {
        deimos_xlate_to_ascii(args->args[x]);
        currentArgument = (*env)->NewStringUTF(env, args->args[x]);
        if (currentArgument == NULL) {
            deimos_xlate_from_ascii(args->args[x]);
            log_error("Cannot create string for argument %s", args->args[x]);
            return false;
        }
        (*env)->SetObjectArrayElement(env, stringArray, x, currentArgument);
    }

    deimos_xlate_to_ascii(load);
    deimos_xlate_to_ascii(loadparams);

    method = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, loader), load, loadparams);

    if (method == NULL) {
        log_error("Cannot find Daemon Loader \"load\" entry point");
        return false;
    }

    log_debug("Daemon loading...");
    ret = (*env)->CallBooleanMethod(env, loader, method, className, stringArray);
    
    if (ret == FALSE) {
        log_error("Cannot load daemon");
        return false;
    }

    log_debug("Daemon loaded successfully");
    return true;
}

static bool java_call(char *method)
{
    jmethodID _method;

    deimos_xlate_to_ascii(method);
    _method = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, loader), method, "()Z");
    if (_method == NULL) {
        log_error("Cannot found \"%s\" method", method);
        return false;
    }

    return (*env)->CallBooleanMethod(env, loader, _method) == TRUE;
}

/* Call the start method in our daemon loader */
bool java_start(void)
{
    bool result = java_call("resume");
    
    if (result == FALSE) {
        log_error("Cannot start daemon");
        return false;
    }

    log_debug("Daemon started successfully");
    return true;
}

/* Call the stop method in our daemon loader */
bool java_stop(void)
{
    bool result = java_call("pause");
    if (result == FALSE) {
        log_error("Cannot stop daemon");
        return false;
    }
    log_debug("Daemon stopped successfully");
    return true;
}

/* Call the destroy method in our daemon loader */
bool java_destroy()
{
    bool result = java_call("destroy");
    if (result == FALSE) {
        log_error("Cannot destroy daemon");
        return false;
    }
    log_debug("Daemon destroy successfully");
    return true;
}

/*
 * call the java sleep to prevent problems with threads
 */
void java_sleep(int wait)
{
    jclass clsThread;
    jmethodID method;
    char jsleep[] = "sleep";
    char jsleepparams[] = "(J)V";
    char jthread[] = "java/lang/Thread";

    deimos_xlate_to_ascii(jsleep);
    deimos_xlate_to_ascii(jsleepparams);
    deimos_xlate_to_ascii(jthread);

    clsThread = (*env)->FindClass(env, jthread);
    if (clsThread == NULL) {
        log_error("Cannot find java/lang/Thread class");
        return;
    }
    method = (*env)->GetStaticMethodID(env, clsThread, jsleep, jsleepparams);
    if (method == NULL) {
        log_error("Cannot found the sleep entry point");
        return;
    }

    (*env)->CallVoidMethod(env, clsThread, method, (jlong) wait * 1000);
}

/* Call the version method in our daemon loader */
bool java_version(void)
{
    jmethodID method;
    char version[] = "version";
    char versionparams[] = "()V";

    deimos_xlate_to_ascii(version);
    deimos_xlate_to_ascii(versionparams);
    method = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, loader), version, versionparams);
    if (method == NULL) {
        log_error("Cannot found Daemon Loader \"version\" entry point");
        return false;
    }

    (*env)->CallVoidMethod(env, loader, method);
    return true;
}

/* Call the check method in our wrapper class */
bool java_check(arg_data *args)
{
    jstring className = NULL;
    jmethodID method = NULL;
    jboolean ret = FALSE;
    char check[] = "check";
    char checkparams[] = "(Ljava/lang/String;)Z";

    log_debug("Checking wrapper");

    deimos_xlate_to_ascii(args->jar);
    className = (*env)->NewStringUTF(env, args->jar);
    deimos_xlate_from_ascii(args->jar);
    if (className == NULL) {
        log_error("Cannot create string for class name");
        return false;
    }

    deimos_xlate_to_ascii(check);
    deimos_xlate_to_ascii(checkparams);
    method = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, loader), check, checkparams);
    if (method == NULL) {
        log_error("Cannot found Daemon Loader \"check\" entry point");
        return false;
    }

    ret = (*env)->CallBooleanMethod(env, loader, method, className);
    if (ret == FALSE) {
        log_error("An error was detected checking the daemon provided in jar %s", args->jar);
        return false;
    }

    log_debug("Daemon checked successfully");
    return true;
}


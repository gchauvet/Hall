/*
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "apxwin.h"
#include "handles.h"
#include "javajni.h"
#include "private.h"

#include <jni.h>

#ifndef JNI_VERSION_1_2
#error -------------------------------------------------------
#error JAVA 1.1 IS NO LONGER SUPPORTED
#error -------------------------------------------------------
#endif

#ifdef JNI_VERSION_1_4
#define JNI_VERSION_DEFAULT JNI_VERSION_1_4
#else
#define JNI_VERSION_DEFAULT JNI_VERSION_1_2
#endif

/* Standard jvm.dll prototypes
 * since only single jvm can exist per process
 * make those global
 */

DYNOLAD_TYPE_DECLARE(JNI_GetDefaultJavaVMInitArgs, JNICALL, jint)(void *);
static DYNLOAD_FPTR_DECLARE(JNI_GetDefaultJavaVMInitArgs) = NULL;

DYNOLAD_TYPE_DECLARE(JNI_CreateJavaVM, JNICALL, jint)(JavaVM **, void **, void *);
static DYNLOAD_FPTR_DECLARE(JNI_CreateJavaVM) = NULL;

DYNOLAD_TYPE_DECLARE(JNI_GetCreatedJavaVMs, JNICALL, jint)(JavaVM **, jsize, jsize *);
static DYNLOAD_FPTR_DECLARE(JNI_GetCreatedJavaVMs) = NULL;

DYNOLAD_TYPE_DECLARE(JVM_DumpAllStacks, JNICALL, void)(JNIEnv *, jclass);
static DYNLOAD_FPTR_DECLARE(JVM_DumpAllStacks) = NULL;

static HANDLE  _st_sys_jvmDllHandle = NULL;
static JavaVM *_st_sys_jvm = NULL;

DYNOLAD_TYPE_DECLARE(SetDllDirectoryW, WINAPI, BOOL)(LPCWSTR);
static DYNLOAD_FPTR_DECLARE(SetDllDirectoryW) = NULL;

#define JVM_GET_OBJECT_CLASS(jvm, cl) (*((jvm)->lpEnv))->GetObjectClass((jvm)->lpEnv, (jvm)->cl)

#define JVM_DELETE_CLAZZ(jvm, cl)                                               \
    APXMACRO_BEGIN                                                              \
    if ((jvm)->lpEnv && (jvm)->cl) {                                   \
        (*((jvm)->lpEnv))->DeleteGlobalRef((jvm)->lpEnv, (*((jvm)->lpEnv))->GetObjectClass((jvm)->lpEnv, (jvm)->cl));   \
    } APXMACRO_END

#define JVM_EXCEPTION_CHECK(jvm) \
    ((*((jvm)->lpEnv))->ExceptionCheck((jvm)->lpEnv) != JNI_OK)

#define JVM_DEFINE_CLASS(jvm, name, size, content) \
    (*((jvm)->lpEnv))->DefineClass((jvm)->lpEnv, name, NULL, content, size)

#define JVM_EXCEPTION_CLEAR(jvm) \
    APXMACRO_BEGIN                                              \
    if ((jvm)->lpEnv) {                                         \
        if ((*((jvm)->lpEnv))->ExceptionCheck((jvm)->lpEnv)) {  \
            (*((jvm)->lpEnv))->ExceptionDescribe((jvm)->lpEnv); \
            (*((jvm)->lpEnv))->ExceptionClear((jvm)->lpEnv);    \
        }                                                       \
    } APXMACRO_END

#define JNI_LOCAL_UNREF(obj) \
        (*(lpJava->lpEnv))->DeleteLocalRef(lpJava->lpEnv, obj)

#define JNICALL_0(fName)  \
        ((*(lpJava->lpEnv))->fName(lpJava->lpEnv))

#define JNICALL_1(fName, a1)  \
        ((*(lpJava->lpEnv))->fName(lpJava->lpEnv, (a1)))

#define JNICALL_2(fName, a1, a2)  \
        ((*(lpJava->lpEnv))->fName(lpJava->lpEnv, (a1), (a2)))

#define JNICALL_3(fName, a1, a2, a3)  \
        ((*(lpJava->lpEnv))->fName(lpJava->lpEnv, (a1), (a2), (a3)))

#define JNICALL_4(fName, a1, a2, a3, a4)  \
        ((*(lpJava->lpEnv))->fName(lpJava->lpEnv, (a1), (a2), (a3), (a4)))

typedef struct {
    DWORD           dwOptions;
    jobject         jWrapper;
    jint            iVersion;
    jsize           iVmCount;
    JNIEnv          *lpEnv;
    JavaVM          *lpJvm;
    /* JVM worker thread info */
    HANDLE          hWorkerThread;
    DWORD           iWorkerThread;
    DWORD           dwWorkerStatus;
    SIZE_T          szStackSize;
    HANDLE          hWorkerSync;
    HANDLE          hWorkerInit;
} APXJAVAVM, *LPAPXJAVAVM;

/* This is no longer exported in jni.h
 * However java uses it internally to get
 * the default stack size
 */
typedef struct APX_JDK1_1InitArgs {
    jint version;

    char **properties;
    jint checkSource;
    jint nativeStackSize;
    jint javaStackSize;
    jint minHeapSize;
    jint maxHeapSize;
    jint verifyMode;
    char *classpath;

    char padding[128];
} APX_JDK1_1InitArgs;

#define JAVA_CLASSPATH      "-Djava.class.path="
#define JAVA_CLASSPATH_W    L"-Djava.class.path="
#define JAVA_CLASSSTRING    "java/lang/String"
#define MSVCRT71_DLLNAME    L"\\msvcrt71.dll"

static DWORD vmExitCode = 0;

static __inline BOOL __apxJvmAttachEnv(LPAPXJAVAVM lpJava, JNIEnv **lpEnv,
                                       LPBOOL lpAttached)
{
    jint _iStatus;

    if (!_st_sys_jvm || !lpJava->lpJvm)
      return FALSE;
    _iStatus = (*(lpJava->lpJvm))->GetEnv(lpJava->lpJvm,
                                          (void **)lpEnv,
                                          lpJava->iVersion);
    if (_iStatus != JNI_OK) {
        if (_iStatus == JNI_EDETACHED) {
            _iStatus = (*(lpJava->lpJvm))->AttachCurrentThread(lpJava->lpJvm,
                                                (void **)lpEnv, NULL);
            if (lpAttached)
                *lpAttached = TRUE;
        }
    }
    if (_iStatus != JNI_OK) {
        *lpEnv = NULL;
        return FALSE;
    }
    else
        return TRUE;
}

static __inline BOOL __apxJvmAttach(LPAPXJAVAVM lpJava)
{
    return __apxJvmAttachEnv(lpJava, &lpJava->lpEnv, NULL);
}

static __inline BOOL __apxJvmDetach(LPAPXJAVAVM lpJava)
{
    if (!_st_sys_jvm || !lpJava->lpJvm)
      return FALSE;
    if ((*(lpJava->lpJvm))->DetachCurrentThread(lpJava->lpJvm) != JNI_OK) {
        lpJava->lpEnv = NULL;
        return FALSE;
    }
    else
        return TRUE;
}

static BOOL __apxLoadJvmDll(LPCWSTR szJvmDllPath)
{
    UINT errMode;
    WCHAR  jreAltPath[SIZ_PATHLEN];
    LPWSTR dllJvmPath = (LPWSTR)szJvmDllPath;
    DYNLOAD_FPTR_DECLARE(SetDllDirectoryW);

    if (!IS_INVALID_HANDLE(_st_sys_jvmDllHandle))
        return TRUE;    /* jvm.dll is already loaded */

    if (dllJvmPath && *dllJvmPath) {
        /* Explicit JVM path.
         * Check if provided argument is valid
         */
        if (GetFileAttributesW(dllJvmPath) == INVALID_FILE_ATTRIBUTES) {
            /* DAEMON-247: Invalid RuntimeLib explicitly specified is error.
             */
            apxLogWrite(APXLOG_MARK_DEBUG "Invalid RuntimeLib specified '%S'", dllJvmPath);
            return FALSE;
        }
    }
    else {
        dllJvmPath = apxGetJavaSoftRuntimeLib(NULL);
        if (!dllJvmPath)
            return FALSE;
    }
    if (GetFileAttributesW(dllJvmPath) == INVALID_FILE_ATTRIBUTES) {
        /* DAEMON-184: RuntimeLib registry key is invalid.
         * Check from Jre JavaHome directly
         */
        LPWSTR szJreHome = apxGetJavaSoftHome(NULL, TRUE);
        apxLogWrite(APXLOG_MARK_DEBUG "Invalid RuntimeLib '%S'", dllJvmPath);
        if (szJreHome) {
            apxLogWrite(APXLOG_MARK_DEBUG "Using Jre JavaHome '%S'", szJreHome);
            lstrlcpyW(jreAltPath, SIZ_PATHLEN, szJreHome);
            lstrlcatW(jreAltPath, SIZ_PATHLEN, L"\\bin\\server\\jvm.dll");
            dllJvmPath = jreAltPath;
        }
    }
    /* Suppress the not found system popup message */
    errMode = SetErrorMode(SEM_FAILCRITICALERRORS);

    apxLogWrite(APXLOG_MARK_DEBUG "loading jvm '%S'", dllJvmPath);
    _st_sys_jvmDllHandle = LoadLibraryExW(dllJvmPath, NULL, 0);
    if (IS_INVALID_HANDLE(_st_sys_jvmDllHandle) &&
        GetFileAttributesW(dllJvmPath) != INVALID_FILE_ATTRIBUTES) {
        /* There is a file but cannot be loaded.
         * Try to load the MSVCRTxx.dll before JVM.dll
         */
        WCHAR  jreBinPath[SIZ_PATHLEN];
        WCHAR  crtBinPath[SIZ_PATHLEN];
        DWORD  i, l = 0;

        lstrlcpyW(jreBinPath, SIZ_PATHLEN, dllJvmPath);
        for (i = lstrlenW(jreBinPath); i > 0, l < 2; i--) {
            if (jreBinPath[i] == L'\\' || jreBinPath[i] == L'/') {
                jreBinPath[i] = L'\0';
                lstrlcpyW(crtBinPath, SIZ_PATHLEN, jreBinPath);
                lstrlcatW(crtBinPath, SIZ_PATHLEN, MSVCRT71_DLLNAME);
                if (GetFileAttributesW(crtBinPath) != INVALID_FILE_ATTRIBUTES) {
                    if (LoadLibraryW(crtBinPath)) {
                        /* Found MSVCRTxx.dll
                         */
                        apxLogWrite(APXLOG_MARK_DEBUG "preloaded '%S'",
                                    crtBinPath);
                        break;
                    }
                }
                l++;
            }
        }
    }
    /* This shuldn't happen, but try to search in %PATH% */
    if (IS_INVALID_HANDLE(_st_sys_jvmDllHandle))
        _st_sys_jvmDllHandle = LoadLibraryExW(dllJvmPath, NULL,
                                              LOAD_WITH_ALTERED_SEARCH_PATH);

    if (IS_INVALID_HANDLE(_st_sys_jvmDllHandle)) {
        WCHAR  jreBinPath[SIZ_PATHLEN];
        DWORD  i, l = 0;

        lstrlcpyW(jreBinPath, SIZ_PATHLEN, dllJvmPath);
        DYNLOAD_FPTR_ADDRESS(SetDllDirectoryW, KERNEL32);
        for (i = lstrlenW(jreBinPath); i > 0, l < 2; i--) {
            if (jreBinPath[i] == L'\\' || jreBinPath[i] == L'/') {
                jreBinPath[i] = L'\0';
                DYNLOAD_CALL(SetDllDirectoryW)(jreBinPath);
                apxLogWrite(APXLOG_MARK_DEBUG "Setting DLL search path to '%S'",
                            jreBinPath);
                l++;
            }
        }
        _st_sys_jvmDllHandle = LoadLibraryExW(dllJvmPath, NULL, 0);
        if (IS_INVALID_HANDLE(_st_sys_jvmDllHandle))
            _st_sys_jvmDllHandle = LoadLibraryExW(dllJvmPath, NULL,
                                                  LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    /* Restore the error mode signalization */
    SetErrorMode(errMode);
    if (IS_INVALID_HANDLE(_st_sys_jvmDllHandle)) {
        apxLogWrite(APXLOG_MARK_SYSERR);
        return FALSE;
    }
    DYNLOAD_FPTR_LOAD(JNI_GetDefaultJavaVMInitArgs, _st_sys_jvmDllHandle);
    DYNLOAD_FPTR_LOAD(JNI_CreateJavaVM,             _st_sys_jvmDllHandle);
    DYNLOAD_FPTR_LOAD(JNI_GetCreatedJavaVMs,        _st_sys_jvmDllHandle);
    DYNLOAD_FPTR_LOAD(JVM_DumpAllStacks,            _st_sys_jvmDllHandle);

    if (!DYNLOAD_FPTR(JNI_GetDefaultJavaVMInitArgs) ||
        !DYNLOAD_FPTR(JNI_CreateJavaVM) ||
        !DYNLOAD_FPTR(JNI_GetCreatedJavaVMs)) {
        apxLogWrite(APXLOG_MARK_SYSERR);
        FreeLibrary(_st_sys_jvmDllHandle);
        _st_sys_jvmDllHandle = NULL;
        return FALSE;
    }

    /* Real voodo ... */
    return TRUE;
}

static BOOL __apxJavaJniCallback(APXHANDLE hObject, UINT uMsg,
                                 WPARAM wParam, LPARAM lParam)
{
    LPAPXJAVAVM lpJava;
    DWORD       dwJvmRet = 0;

    lpJava = APXHANDLE_DATA(hObject);
    switch (uMsg) {
        case WM_CLOSE:
            if (_st_sys_jvm && lpJava->lpJvm) {
                if (!IS_INVALID_HANDLE(lpJava->hWorkerThread)) {
                    if (GetExitCodeThread(lpJava->hWorkerThread, &dwJvmRet) &&
                        dwJvmRet == STILL_ACTIVE) {
                        TerminateThread(lpJava->hWorkerThread, 5);
                    }
                }
                SAFE_CLOSE_HANDLE(lpJava->hWorkerThread);
                __apxJvmAttach(lpJava);
                JVM_DELETE_CLAZZ(lpJava, jWrapper);
                __apxJvmDetach(lpJava);
                /* Check if this is the jvm loader */
                if (!lpJava->iVmCount && _st_sys_jvmDllHandle) {
                    /* Unload JVM dll */
                    FreeLibrary(_st_sys_jvmDllHandle);
                    _st_sys_jvmDllHandle = NULL;
                }
                lpJava->lpJvm = NULL;
            }
        break;
        default:
        break;
    }
    return TRUE;
}

static __inline const char *__apxJvmGetClassName(LPAPXJAVAVM lpJava, jobject obj)
{
    jclass cls = JNICALL_1(GetObjectClass, obj);
    jmethodID mid = JNICALL_3(GetMethodID, cls, "getClass", "()Ljava/lang/Class;");
    jobject clsObj = JNICALL_2(CallObjectMethod, obj, mid);
    cls = JNICALL_1(GetObjectClass, clsObj);
    mid = JNICALL_3(GetMethodID, cls, "getName", "()Ljava/lang/String;");
    jstring strObj = (jstring) JNICALL_2(CallObjectMethod, clsObj, mid);
    return JNICALL_2(GetStringUTFChars, strObj, NULL);
}

APXHANDLE
apxCreateJava(APXHANDLE hPool, LPCWSTR szJvmDllPath)
{
    APXHANDLE    hJava;
    LPAPXJAVAVM  lpJava;
    jsize        iVmCount;
    JavaVM       *lpJvm = NULL;
    struct       APX_JDK1_1InitArgs jArgs1_1;

    if (!__apxLoadJvmDll(szJvmDllPath))
        return NULL;

    if (DYNLOAD_FPTR(JNI_GetCreatedJavaVMs)(&lpJvm, 1, &iVmCount) != JNI_OK) {
        return NULL;
    }
    if (iVmCount && !lpJvm)
        return NULL;

    hJava = apxHandleCreate(hPool, 0,
                            NULL, sizeof(APXJAVAVM),
                            __apxJavaJniCallback);
    if (IS_INVALID_HANDLE(hJava))
        return NULL;
    hJava->dwType = APXHANDLE_TYPE_JVM;
    lpJava = APXHANDLE_DATA(hJava);
    lpJava->lpJvm = lpJvm;
    lpJava->iVmCount = iVmCount;

    /* Guess the stack size
     */
    AplZeroMemory(&jArgs1_1, sizeof(jArgs1_1));
    jArgs1_1.version = JNI_VERSION_1_1;
    DYNLOAD_FPTR(JNI_GetDefaultJavaVMInitArgs)(&jArgs1_1);
    if (jArgs1_1.javaStackSize < 0 || jArgs1_1.javaStackSize > (2048 * 1024))
        jArgs1_1.javaStackSize = 0;
    lpJava->szStackSize = (SIZE_T)jArgs1_1.javaStackSize;

    if (!_st_sys_jvm)
        _st_sys_jvm = lpJvm;
    return hJava;
}

static DWORD WINAPI __apxJavaDestroyThread(LPVOID lpParameter)
{
    JavaVM *lpJvm = (JavaVM *)lpParameter;
    (*lpJvm)->DestroyJavaVM(lpJvm);
    return 0;
}

BOOL
apxDestroyJvm(DWORD dwTimeout)
{
    if (_st_sys_jvm) {
        DWORD  tid;
        HANDLE hWaiter;
        BOOL   rv = FALSE;
        JavaVM *lpJvm = _st_sys_jvm;

        _st_sys_jvm = NULL;
        (*lpJvm)->DetachCurrentThread(lpJvm);
        hWaiter = CreateThread(NULL, 0, __apxJavaDestroyThread,
                               (void *)lpJvm, 0, &tid);
        if (IS_INVALID_HANDLE(hWaiter)) {
            apxLogWrite(APXLOG_MARK_SYSERR);
            return FALSE;
        }
        if (WaitForSingleObject(hWaiter, dwTimeout) == WAIT_OBJECT_0)
            rv = TRUE;
        CloseHandle(hWaiter);
        return rv;
    }
    else
        return FALSE;
}

static DWORD __apxMultiSzToJvmOptions(APXHANDLE hPool,
                                      LPCSTR lpString,
                                      JavaVMOption **lppArray,
                                      DWORD  nExtra)
{
    DWORD i, n = 0, l = 0;
    char *buff;
    LPSTR p;

    if (lpString) {
        l = __apxGetMultiSzLengthA(lpString, &n);
    }
    n += nExtra;
    buff = apxPoolAlloc(hPool, (n + 1) * sizeof(JavaVMOption) + (l + 1));

    *lppArray = (JavaVMOption *)buff;
    p = (LPSTR)(buff + (n + 1) * sizeof(JavaVMOption));
    if (lpString)
        AplCopyMemory(p, lpString, l + 1);
    for (i = 0; i < (n - nExtra); i++) {
        DWORD qr = apxStrUnQuoteInplaceA(p);
        (*lppArray)[i].optionString = p;
        while (*p)
            p++;
        p++;
        p += qr;
    }

    return n;
}

/* a hook for a function that redirects all VM messages. */
static jint JNICALL __apxJniVfprintf(FILE *fp, const char *format, va_list args)
{
    jint rv;
    CHAR sBuf[1024+16];
    rv = wvsprintfA(sBuf, format, args);
    if (apxLogWrite(APXLOG_MARK_INFO "%s", sBuf) == 0)
        fputs(sBuf, stdout);
    return rv;
}

static void JNICALL __apxJniExit(jint exitCode)
{
    apxLogWrite(APXLOG_MARK_DEBUG "Exit hook with exit code %d", exitCode);
    vmExitCode = exitCode;
    return;
}

static LPSTR __apxStrIndexA(LPCSTR szStr, int nCh)
{
    LPSTR pStr;

    for (pStr = (LPSTR)szStr; *pStr; pStr++) {
        if (*pStr == nCh)
            return pStr;
    }
    return NULL;
}

static LPSTR __apxStrnCatA(APXHANDLE hPool, LPSTR pOrg, LPCSTR szStr, LPCSTR szAdd)
{
    DWORD len = 1;
    DWORD nas = pOrg == NULL;
    if (pOrg)
        len += lstrlenA(pOrg);
    if (szStr)
        len += lstrlenA(szStr);
    if (szAdd)
        len += lstrlenA(szAdd);
    pOrg = (LPSTR)apxPoolRealloc(hPool, pOrg, len);
    if (pOrg) {
        if (nas)
            *pOrg = '\0';
        if (szStr)
            lstrcatA(pOrg, szStr);
        if (szAdd)
            lstrcatA(pOrg, szAdd);
    }
    return pOrg;
}

static LPSTR __apxEvalPathPart(APXHANDLE hPool, LPSTR pStr, LPCSTR szPattern)
{
    HANDLE           hFind;
    WIN32_FIND_DATAA stGlob;
    char       szJars[MAX_PATH + 1];
    char       szPath[MAX_PATH + 1];

    if (lstrlenA(szPattern) > (sizeof(szJars) - 5)) {
        return __apxStrnCatA(hPool, pStr, szPattern, NULL);
    }
    lstrcpyA(szJars, szPattern);
    szPath[0] = ';';
    szPath[1] = '\0';
    lstrcatA(szPath, szPattern);
    lstrcatA(szJars, ".jar");
    /* Remove the trailing asterisk
     */
    szPath[lstrlenA(szPath) - 1] = '\0';
    if ((hFind = FindFirstFileA(szJars, &stGlob)) == INVALID_HANDLE_VALUE) {
        /* Find failed
         */
        return pStr;
    }
    pStr = __apxStrnCatA(hPool, pStr, &szPath[1], stGlob.cFileName);
    if (pStr == NULL) {
        FindClose(hFind);
        return NULL;
    }
    while (FindNextFileA(hFind, &stGlob) != 0) {
        pStr = __apxStrnCatA(hPool, pStr, szPath, stGlob.cFileName);
        if (pStr == NULL)
            break;
    }
    FindClose(hFind);
    return pStr;
}

/**
 * Call glob on each PATH like string path.
 * Glob is called only if the part ends with asterisk in which
 * case asterisk is replaced by *.jar when searching
 */
static LPSTR __apxEvalClasspath(APXHANDLE hPool, LPCSTR szCp)
{
    LPSTR pCpy = __apxStrnCatA(hPool, NULL, JAVA_CLASSPATH, szCp);
    LPSTR pGcp = NULL;
    LPSTR pPos;
    LPSTR pPtr;

    if (!pCpy)
        return NULL;
    pPtr = pCpy + sizeof(JAVA_CLASSPATH) - 1;
    while ((pPos = __apxStrIndexA(pPtr, ';'))) {
        *pPos = '\0';
        if (pGcp)
            pGcp = __apxStrnCatA(hPool, pGcp, ";", NULL);
        else
            pGcp = __apxStrnCatA(hPool, NULL, JAVA_CLASSPATH, NULL);
        if ((pPos > pPtr) && (*(pPos - 1) == '*')) {
            if (!(pGcp = __apxEvalPathPart(hPool, pGcp, pPtr))) {
                /* Error.
                * Return the original string processed so far.
                */
                return pCpy;
            }
        }
        else {
            /* Standard path element */
            if (!(pGcp = __apxStrnCatA(hPool, pGcp, pPtr, NULL))) {
                /* Error.
                * Return the original string processed so far.
                */
                return pCpy;
            }
        }
        pPtr = pPos + 1;
    }
    if (*pPtr) {
        int end = lstrlenA(pPtr);
        if (pGcp)
            pGcp = __apxStrnCatA(hPool, pGcp, ";", NULL);
        else
            pGcp = __apxStrnCatA(hPool, NULL, JAVA_CLASSPATH, NULL);
        if (end > 0 && pPtr[end - 1] == '*') {
            /* Last path elemet ends with star
             * Do a globbing.
             */
            pGcp = __apxEvalPathPart(hPool, pGcp, pPtr);
        }
        else {
            /* Just add the part */
            pGcp = __apxStrnCatA(hPool, pGcp, pPtr, NULL);
        }
    }
    /* Free the allocated copy */
    if (pGcp) {
        apxFree(pCpy);
        return pGcp;
    }
    else
        return pCpy;
}

/* ANSI version only */
static BOOL
apxJavaInitialize(APXHANDLE hJava, LPCSTR szClassPath,
                  LPCVOID lpOptions, DWORD dwMs, DWORD dwMx,
                  DWORD dwSs, DWORD bJniVfprintf)
{
    LPAPXJAVAVM     lpJava;
    JavaVMInitArgs  vmArgs;
    JavaVMOption    *lpJvmOptions;
    DWORD           i, nOptions, sOptions = 0;
    BOOL            rv = FALSE;
    if (hJava->dwType != APXHANDLE_TYPE_JVM)
        return FALSE;

    lpJava = APXHANDLE_DATA(hJava);

    if (lpJava->iVmCount) {
        if (!lpJava->lpEnv && !__apxJvmAttach(lpJava)) {
            if (lpJava->iVersion == JNI_VERSION_1_2) {
                apxLogWrite(APXLOG_MARK_ERROR "Unable To Attach the JVM");
                return FALSE;
            }
            else
                lpJava->iVersion = JNI_VERSION_1_2;
            if (!__apxJvmAttach(lpJava)) {
                apxLogWrite(APXLOG_MARK_ERROR "Unable To Attach the JVM");
                return FALSE;
            }
        }
        lpJava->iVersion = JNICALL_0(GetVersion);
        if (lpJava->iVersion < JNI_VERSION_1_2) {
            apxLogWrite(APXLOG_MARK_ERROR "Unsupported JNI version %#08x", lpJava->iVersion);
            return FALSE;
        }
        rv = TRUE;
    } else {
        CHAR  iB[3][64];
        LPSTR szCp = NULL;
        lpJava->iVersion = JNI_VERSION_DEFAULT;

        if (dwMs)
            ++sOptions;
        if (dwMx)
            ++sOptions;
        if (dwSs)
            ++sOptions;
        if (bJniVfprintf)
            ++sOptions;
        if (szClassPath && *szClassPath)
            ++sOptions;

        sOptions++; /* unconditionally set for extraInfo exit */

        nOptions = __apxMultiSzToJvmOptions(hJava->hPool, lpOptions,
                                            &lpJvmOptions, sOptions);
        if (szClassPath && *szClassPath) {
            szCp = __apxEvalClasspath(hJava->hPool, szClassPath);
            if (szCp == NULL) {
                apxLogWrite(APXLOG_MARK_ERROR "Invalid classpath %s", szClassPath);
                return FALSE;
            }
            lpJvmOptions[nOptions - sOptions].optionString = szCp;
            --sOptions;
        }
        if (bJniVfprintf) {
            /* default JNI error printer */
            lpJvmOptions[nOptions - sOptions].optionString = "vfprintf";
            lpJvmOptions[nOptions - sOptions].extraInfo    = __apxJniVfprintf;
            --sOptions;
        }

        /* unconditionally add hook for System.exit() in order to store exit code */
        lpJvmOptions[nOptions - sOptions].optionString = "exit";
        lpJvmOptions[nOptions - sOptions].extraInfo    = __apxJniExit;
        --sOptions;

        if (dwMs) {
            wsprintfA(iB[0], "-Xms%dm", dwMs);
            lpJvmOptions[nOptions - sOptions].optionString = iB[0];
            --sOptions;
        }
        if (dwMx) {
            wsprintfA(iB[1], "-Xmx%dm", dwMx);
            lpJvmOptions[nOptions - sOptions].optionString = iB[1];
            --sOptions;
        }
        if (dwSs) {
            wsprintfA(iB[2], "-Xss%dk", dwSs);
            lpJvmOptions[nOptions - sOptions].optionString = iB[2];
            --sOptions;
        }
        for (i = 0; i < nOptions; i++) {
            apxLogWrite(APXLOG_MARK_DEBUG "Jvm Option[%d] %s", i,
                        lpJvmOptions[i].optionString);
        }
        vmArgs.options  = lpJvmOptions;
        vmArgs.nOptions = nOptions;
        vmArgs.version  = lpJava->iVersion;
        vmArgs.ignoreUnrecognized = JNI_FALSE;
        
        if (DYNLOAD_FPTR(JNI_CreateJavaVM)(&(lpJava->lpJvm),
                                           (void **)&(lpJava->lpEnv),
                                           &vmArgs) != JNI_OK) {
            apxLogWrite(APXLOG_MARK_ERROR "CreateJavaVM Failed");
            rv = FALSE;
        } else {
            rv = TRUE;
            if (!_st_sys_jvm)
                _st_sys_jvm = lpJava->lpJvm;
        }
        apxFree(szCp);
        apxFree(lpJvmOptions);
    }
    if (rv)
        return TRUE;
    else
        return FALSE;
}

static BOOL
apxJavaLoadMainClass(APXHANDLE hJava, LPCSTR szJarName, LPCVOID lpArguments)
{
    LPWSTR      *lpArgs = NULL;
    DWORD       nArgs;
    jarray      jArgs;
    LPAPXJAVAVM lpJava;
    jmethodID   method;
    
    if (hJava->dwType != APXHANDLE_TYPE_JVM)
        return FALSE;
    lpJava = APXHANDLE_DATA(hJava);
    if (!lpJava)
        return FALSE;
    
    nArgs = apxMultiSzToArrayW(hJava->hPool, lpArguments, &lpArgs);
    jArgs = JNICALL_3(NewObjectArray, nArgs, JNICALL_1(FindClass, "java/lang/String"), NULL);
    if (nArgs) {
        for (DWORD i = 0; i < nArgs; i++) {
            jstring arg = JNICALL_2(NewString, lpArgs[i], lstrlenW(lpArgs[i]));
            JNICALL_3(SetObjectArrayElement, jArgs, i, arg);
            apxLogWrite(APXLOG_MARK_DEBUG "argv[%d] = %S", i, lpArgs[i]);
        }
    }
    apxFree(lpArgs);
    
    method = JNICALL_3(GetMethodID, JVM_GET_OBJECT_CLASS(lpJava, jWrapper), "load", "(Ljava/lang/String;[Ljava/lang/String;)Z");
    if (method == NULL) {
        apxLogWrite(APXLOG_MARK_ERROR, "Cannot find \"load\" entry point");
        return FALSE;
    }
    return JNICALL_4(CallBooleanMethod, lpJava->jWrapper, method, JNICALL_1(NewStringUTF, szJarName), jArgs) == JNI_TRUE ? TRUE : FALSE;
}

static DWORD
apxJavaSetOptions(APXHANDLE hJava, DWORD dwOptions)
{
    DWORD dwOrgOptions;
    LPAPXJAVAVM lpJava;

    if (hJava->dwType != APXHANDLE_TYPE_JVM)
        return 0;
    lpJava = APXHANDLE_DATA(hJava);
    dwOrgOptions = lpJava->dwOptions;
    lpJava->dwOptions = dwOptions;
    return dwOrgOptions;
}

DWORD
apxJavaWait(APXHANDLE hJava, DWORD dwMilliseconds, BOOL bKill)
{
    DWORD rv;
    LPAPXJAVAVM lpJava;

    if (hJava->dwType != APXHANDLE_TYPE_JVM)
        return FALSE;
    lpJava = APXHANDLE_DATA(hJava);

    if (!lpJava->dwWorkerStatus && lpJava->hWorkerThread)
        return WAIT_OBJECT_0;
    rv = WaitForSingleObject(lpJava->hWorkerThread, dwMilliseconds);
    if (rv == WAIT_TIMEOUT && bKill) {
        __apxJavaJniCallback(hJava, WM_CLOSE, 0, 0);
    }

    return rv;
}

LPVOID
apxJavaCreateClassV(APXHANDLE hJava, LPCSTR szClassName,
                    LPCSTR szSignature, va_list lpArgs)
{
    LPAPXJAVAVM     lpJava;
    jclass          clazz;
    jmethodID       ccont;
    jobject         cinst;

    if (hJava->dwType != APXHANDLE_TYPE_JVM)
        return NULL;
    lpJava = APXHANDLE_DATA(hJava);
    if (!__apxJvmAttach(lpJava))
        return NULL;

    clazz = JNICALL_1(FindClass, szClassName);
    if (clazz == NULL || (JVM_EXCEPTION_CHECK(lpJava))) {
        JVM_EXCEPTION_CLEAR(lpJava);
        apxLogWrite(APXLOG_MARK_ERROR "Could not FindClass %s", szClassName);
        return NULL;
    }

    ccont = JNICALL_3(GetMethodID, clazz, "<init>", szSignature);
    if (ccont == NULL || (JVM_EXCEPTION_CHECK(lpJava))) {
        JVM_EXCEPTION_CLEAR(lpJava);
        apxLogWrite(APXLOG_MARK_ERROR "Could not find Constructor %s for %s",
                    szSignature, szClassName);
        return NULL;
    }

    cinst = JNICALL_3(NewObjectV, clazz, ccont, lpArgs);
    if (cinst == NULL || (JVM_EXCEPTION_CHECK(lpJava))) {
        JVM_EXCEPTION_CLEAR(lpJava);
        apxLogWrite(APXLOG_MARK_ERROR "Could not create instance of %s",
                    szClassName);
        return NULL;
    }

    return cinst;
}

static LPVOID
apxJavaCreateClass(APXHANDLE hJava, LPCSTR szClassName,
                   LPCSTR szSignature, ...)
{
    LPVOID rv;
    va_list args;

    va_start(args, szSignature);
    rv = apxJavaCreateClassV(hJava, szClassName, szSignature, args);
    va_end(args);

    return rv;
}

static LPVOID
apxJavaCreateStringA(APXHANDLE hJava, LPCSTR szString)
{
    LPAPXJAVAVM     lpJava;
    jstring str;

    if (hJava->dwType != APXHANDLE_TYPE_JVM)
        return NULL;
    lpJava = APXHANDLE_DATA(hJava);

    str = JNICALL_1(NewStringUTF, szString);
    if (str == NULL || (JVM_EXCEPTION_CHECK(lpJava))) {
        JVM_EXCEPTION_CLEAR(lpJava);
        apxLogWrite(APXLOG_MARK_ERROR "Could not create string for %s",
                    szString);
        return NULL;
    }

    return str;
}

static LPVOID
apxJavaCreateStringW(APXHANDLE hJava, LPCWSTR szString)
{
    LPAPXJAVAVM     lpJava;
    jstring str;

    if (hJava->dwType != APXHANDLE_TYPE_JVM)
        return NULL;
    lpJava = APXHANDLE_DATA(hJava);

    str = JNICALL_2(NewString, szString, lstrlenW(szString));
    if (str == NULL || (JVM_EXCEPTION_CHECK(lpJava))) {
        JVM_EXCEPTION_CLEAR(lpJava);
        apxLogWrite(APXLOG_MARK_ERROR "Could not create string for %S",
                    szString);
        return NULL;
    }

    return str;
}

static jvalue
apxJavaCallStaticMethodV(APXHANDLE hJava, jclass lpClass, LPCSTR szMethodName,
                         LPCSTR szSignature, va_list lpArgs)
{
    LPAPXJAVAVM     lpJava;
    jmethodID       method;
    jvalue          rv;
    LPCSTR          s = szSignature;
    rv.l = 0;
    if (hJava->dwType != APXHANDLE_TYPE_JVM)
        return rv;
    lpJava = APXHANDLE_DATA(hJava);

    while (*s && *s != ')')
        ++s;
    if (*s != ')') {
        return rv;
    }
    else
        ++s;
    method = JNICALL_3(GetStaticMethodID, lpClass, szMethodName, szSignature);
    if (method == NULL || (JVM_EXCEPTION_CHECK(lpJava))) {
        JVM_EXCEPTION_CLEAR(lpJava);
        apxLogWrite(APXLOG_MARK_ERROR "Could not find method %s with signature %s",
                    szMethodName, szSignature);
        return rv;
    }
    switch (*s) {
        case 'V':
            JNICALL_3(CallStaticVoidMethodV, lpClass, method, lpArgs);
        break;
        case 'L':
        case '[':
            rv.l = JNICALL_3(CallStaticObjectMethodV, lpClass, method, lpArgs);
        break;
        case 'Z':
            rv.z = JNICALL_3(CallStaticBooleanMethodV, lpClass, method, lpArgs);
        break;
        case 'B':
            rv.b = JNICALL_3(CallStaticByteMethodV, lpClass, method, lpArgs);
        break;
        case 'C':
            rv.c = JNICALL_3(CallStaticCharMethodV, lpClass, method, lpArgs);
        break;
        case 'S':
            rv.i = JNICALL_3(CallStaticShortMethodV, lpClass, method, lpArgs);
        break;
        case 'I':
            rv.i = JNICALL_3(CallStaticIntMethodV, lpClass, method, lpArgs);
        break;
        case 'J':
            rv.j = JNICALL_3(CallStaticLongMethodV, lpClass, method, lpArgs);
        break;
        case 'F':
            rv.f = JNICALL_3(CallStaticFloatMethodV, lpClass, method, lpArgs);
        break;
        case 'D':
            rv.d = JNICALL_3(CallStaticDoubleMethodV, lpClass, method, lpArgs);
        break;
        default:
            apxLogWrite(APXLOG_MARK_ERROR "Invalid signature %s for method %s",
                        szSignature, szMethodName);
            return rv;
        break;
    }

    return rv;
}

static jvalue
apxJavaCallStaticMethod(APXHANDLE hJava, jclass lpClass, LPCSTR szMethodName,
                        LPCSTR szSignature, ...)
{
    jvalue rv;
    va_list args;

    va_start(args, szSignature);
    rv = apxJavaCallStaticMethodV(hJava, lpClass, szMethodName, szSignature, args);
    va_end(args);

    return rv;
}

/* Call the Java:
 * System.setOut(new PrintStream(new FileOutputStream(filename)));
 */
static BOOL
apxJavaSetOut(APXHANDLE hJava, BOOL setErrorOrOut, LPCWSTR szFilename)
{
    LPAPXJAVAVM lpJava;
    jobject     fs;
    jobject     ps;
    jstring     fn;
    jclass      sys;

    if (hJava->dwType != APXHANDLE_TYPE_JVM || !szFilename)
        return FALSE;
    lpJava = APXHANDLE_DATA(hJava);
    if (!__apxJvmAttach(lpJava))
        return FALSE;

    if ((fn = apxJavaCreateStringW(hJava, szFilename)) == NULL)
        return FALSE;
    if ((fs = apxJavaCreateClass(hJava, "java/io/FileOutputStream",
                                 "(Ljava/lang/String;Z)V", fn, JNI_TRUE)) == NULL)
        return FALSE;
    if ((ps = apxJavaCreateClass(hJava, "java/io/PrintStream",
                                 "(Ljava/io/OutputStream;)V", fs)) == NULL)
        return FALSE;
    sys = JNICALL_1(FindClass, "java/lang/System");
    if (sys == NULL || (JVM_EXCEPTION_CHECK(lpJava))) {
        JVM_EXCEPTION_CLEAR(lpJava);
        apxLogWrite(APXLOG_MARK_ERROR "Could not FindClass java/lang/System");
        return FALSE;
    }

    if (setErrorOrOut)
        apxJavaCallStaticMethod(hJava, sys, "setErr", "(Ljava/io/PrintStream;)V", ps);
    else
        apxJavaCallStaticMethod(hJava, sys, "setOut", "(Ljava/io/PrintStream;)V", ps);

    if (JVM_EXCEPTION_CHECK(lpJava)) {
        JVM_EXCEPTION_CLEAR(lpJava);
        apxLogWrite(APXLOG_MARK_ERROR "Error calling set method for java/lang/System");
        return FALSE;
    }
    else
        return TRUE;
}

DWORD apxGetVmExitCode(void) {
    return vmExitCode;
}

void apxSetVmExitCode(DWORD exitCode) {
    vmExitCode = exitCode;
    return;
}

void
apxJavaDumpAllStacks(APXHANDLE hJava)
{
    BOOL bAttached;
    LPAPXJAVAVM lpJava;
    JNIEnv *lpEnv = NULL;

    if (DYNLOAD_FPTR(JVM_DumpAllStacks) == NULL ||
        hJava == NULL ||
        hJava->dwType != APXHANDLE_TYPE_JVM)
        return;
    lpJava = APXHANDLE_DATA(hJava);
    if (__apxJvmAttachEnv(lpJava, &lpEnv, &bAttached)) {
        DYNLOAD_FPTR(JVM_DumpAllStacks)(lpEnv, NULL);
        if (bAttached)
            (*(lpJava->lpJvm))->DetachCurrentThread(lpJava->lpJvm);
    }
}

BOOL
apxJavaInit(APXHANDLE instance, LAPXJAVA_INIT options)
{
    const LPAPXJAVAVM lpJava = APXHANDLE_DATA(instance);
    if (!lpJava)
        return FALSE;
    
    if (!apxJavaInitialize(instance,
                           options->szClassPath,
                           options->lpOptions,
                           options->dwMs, options->dwMx, options->dwSs,
                           options->bJniVfprintf)) {
        return FALSE;
    }

    if (options->szLibraryPath && *options->szLibraryPath) {
        DYNLOAD_FPTR_ADDRESS(SetDllDirectoryW, KERNEL32);
        DYNLOAD_CALL(SetDllDirectoryW)(options->szLibraryPath);
        apxLogWrite(APXLOG_MARK_DEBUG "DLL search path set to '%S'",
                    options->szLibraryPath);
    }
    
    // Load our embedded classloader and embedded jar
    HRSRC hresCl = FindResource(NULL, MAKEINTRESOURCE(IDD_HALL_CL), RT_RCDATA);
    HRSRC hresJar = FindResource(NULL, MAKEINTRESOURCE(IDD_HALL_JAR), RT_RCDATA);
    HGLOBAL resCl = LoadResource(NULL, hresCl);
    jclass clLoader = JVM_DEFINE_CLASS(lpJava, "io/zatarox/hall/impl/EmbeddedClassLoader", SizeofResource(NULL, hresCl), LockResource(resCl));

    // Load the embedded daemon single jar
    DWORD szJar = SizeofResource(NULL, hresJar);
    HGLOBAL resJar = LoadResource(NULL, hresJar);
    jbyteArray array = (*((lpJava)->lpEnv))->NewByteArray((lpJava)->lpEnv, szJar);
    (*((lpJava)->lpEnv))->SetByteArrayRegion(
        (lpJava)->lpEnv,
        array,
        0,
        szJar,
        LockResource(resJar)
    );

    // Call createBootstrap to get our wrapper implementation.
    lpJava->jWrapper = JNICALL_1(NewGlobalRef,
        JNICALL_3(
            CallStaticObjectMethod,
            clLoader,
            JNICALL_3(
                GetStaticMethodID,
                clLoader,
                "createBootstrap",
                "([B)Ljava/lang/Object;"
            ),
            array
        )
    );
    
    if (!apxJavaLoadMainClass(instance,
                              options->szJarName,
                              options->lpArguments)) {
        return FALSE;
    }
    apxJavaSetOut(instance, TRUE,  options->szStdErrFilename);
    apxJavaSetOut(instance, FALSE, options->szStdOutFilename);
    return TRUE;
}

BOOL
apxJavaCall(APXHANDLE instance, LPCSTR szMethod)
{
    const LPAPXJAVAVM lpJava = APXHANDLE_DATA(instance);
    jmethodID method;
    if (!lpJava)
        return FALSE;
    if (!__apxJvmAttach(lpJava))
        return FALSE;
    
    method = JNICALL_3(GetMethodID, JVM_GET_OBJECT_CLASS(lpJava, jWrapper), szMethod, "()Z");
    JNICALL_2(CallVoidMethod, lpJava->jWrapper, method);
    if (JVM_EXCEPTION_CHECK(lpJava)) {
        apxLogWrite(APXLOG_MARK_DEBUG "Exception has been thrown");
        vmExitCode = 1;
        (*((lpJava)->lpEnv))->ExceptionDescribe((lpJava)->lpEnv);
        __apxJvmDetach(lpJava);
        return FALSE;
    }
    else {
        __apxJvmDetach(lpJava);
        return TRUE;
    }
}
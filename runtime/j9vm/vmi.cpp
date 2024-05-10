/*******************************************************************************
 * Copyright IBM Corp. and others 2024
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

/**
 * @brief  This file contains implementations of the public JVM interface (JVM_ functions)
 * which simply forward to a concrete implementation located either in the JCL library
 * or proxy forwarder.
 */

extern "C" {

#include "../util/ut_module.h"
#include "j9.h"
#include "sunvmi_api.h"
#include <assert.h>
#include <jni.h>
#include <stdlib.h>
#undef UT_MODULE_LOADED
#undef UT_MODULE_UNLOADED
#include "bcverify_api.h"
#include "hashtable_api.h"
#include "j2sever.h"
#include "j9cfg.h"
#include "j9consts.h"
#include "j9jclnls.h"
#include "j9modifiers_api.h"
#include "j9protos.h"
#include "j9version.h"
#include "j9vm_internal.h"
#include "j9vmconstantpool.h"
#include "j9vmnls.h"
#include "jclprots.h"
#include "jvminit.h"
#include "omrgcconsts.h"
#include "rommeth.h"
#include "ut_j9scar.h"
#include "util_api.h"
#include "vm_api.h"

#if JAVA_SPEC_VERSION >= 19
#include "ContinuationHelpers.hpp"
#include "VMHelpers.hpp"
#endif /* JAVA_SPEC_VERSION >= 19 */

extern J9JavaVM *BFUjavaVM;
SunVMI *g_VMI;

constexpr jint POK_BOOLEAN = 4;
constexpr jint POK_CHAR = 5;
constexpr jint POK_FLOAT = 6;
constexpr jint POK_DOUBLE = 7;
constexpr jint POK_BYTE = 8;
constexpr jint POK_SHORT = 9;
constexpr jint POK_INT = 10;
constexpr jint POK_LONG = 11;

#if JAVA_SPEC_VERSION >= 11
constexpr jlong J9TIME_NANOSECONDS_PER_SECOND = 1000000000;
/* Need to do a |currentSecondsTime - secondsOffset| < (2^32) check to ensure that the
 * resulting time fits into a long so it doesn't overflow. This is equivalent to doing
 * |currentNanoTime - nanoTimeOffset| <  4294967295000000000.
 */
constexpr jlong TIME_LONG_MAX = 4294967295000000000LL;
constexpr jlong TIME_LONG_MIN = -4294967295000000000LL;
constexpr jlong OFFSET_MAX = 0x225C17D04LL; /*  2^63/10^9 */
constexpr jlong OFFSET_MIN = 0xFFFFFFFDDA3E82FCLL; /* -2^63/10^9 */

constexpr UDATA HASHTABLE_ATPUT_SUCCESS = 0U;
constexpr UDATA HASHTABLE_ATPUT_GENERAL_FAILURE = 1U;
constexpr UDATA HASHTABLE_ATPUT_COLLISION_FAILURE = 2U;

constexpr U_32 INITIAL_INTERNAL_MODULE_HASHTABLE_SIZE = 1;
constexpr U_32 INITIAL_INTERNAL_PACKAGE_HASHTABLE_SIZE = 1;

/* All the helper functions below assume that:
 * a) If VMAccess is required, it assumes the caller has already done so
 * b) If performing a hash operation, it assumes the caller has already locked vm->classLoaderModuleAndLocationMutex
 */
static UDATA
hashPackageTableDelete(J9VMThread *currentThread, J9ClassLoader *classLoader, const char *packageName);

static J9Package *
createPackage(J9VMThread *currentThread, J9Module *fromModule, const char *package);

static void
freePackageDefinition(J9VMThread *currentThread, J9ClassLoader *classLoader, const char *packageName);

static bool
removePackageDefinition(J9VMThread *currentThread, J9Module *fromModule, const char *packageName);

static bool
addPackageDefinition(J9VMThread *currentThread, J9Module *fromModule, const char *package);

static UDATA
addMulPackageDefinitions(J9VMThread *currentThread,
                         J9Module *fromModule,
                         const char *const *packages,
                         U_32 numPackages);

static void
removeMulPackageDefinitions(J9VMThread *currentThread,
                            J9Module *fromModule,
                            const char *const *packages,
                            U_32 packagesIndex);

static UDATA
addModuleDefinition(J9VMThread *currentThread,
                    J9Module *fromModule,
                    const char *const *packages,
                    U_32 numPackages,
                    jstring version);

static bool
isPackageDefined(J9VMThread *currentThread, J9ClassLoader *classLoader, const char *packageName);

static bool
areNoPackagesDefined(J9VMThread *currentThread,
                     J9ClassLoader *classLoader,
                     const char *const *packages,
                     U_32 numPackages);

static UDATA
exportPackageToAll(J9VMThread *currentThread, J9Module *fromModule, const char *package);

static UDATA
exportPackageToAllUnamed(J9VMThread *currentThread, J9Module *fromModule, const char *package);

static UDATA
exportPackageToModule(J9VMThread *currentThread, J9Module *fromModule, const char *package, J9Module *toModule);

static void
trcModulesCreationPackage(J9VMThread *currentThread, J9Module *fromModule, const char *package);

static void
trcModulesAddModuleExportsToAll(J9VMThread *currentThread, J9Module *fromModule, const char *package);

static void
trcModulesAddModuleExportsToAllUnnamed(J9VMThread *currentThread, J9Module *fromModule, const char *package);

static void
trcModulesAddModuleExports(J9VMThread *currentThread, J9Module *fromModule, const char *package, J9Module *toModule);

static void
trcModulesAddModulePackage(J9VMThread *currentThread, J9Module *j9mod, const char *package);

static UDATA
hashTableAtPut(J9HashTable *table, void *value, bool collisionIsFailure);

static void
throwExceptionHelper(J9VMThread *currentThread, UDATA errCode);

static void
freePackage(J9VMThread *currentThread, J9Package *j9package);

static J9ClassLoader *
getModuleObjectClassLoader(J9VMThread *currentThread, j9object_t moduleObject);

static J9Module *
createModule(J9VMThread *currentThread, j9object_t moduleObject, J9ClassLoader *classLoader, j9object_t moduleName);

static J9Module *
getJ9Module(J9VMThread *currentThread, jobject module);

static bool
isModuleNameValid(j9object_t moduleName);

static bool
isModuleJavaBase(j9object_t moduleName);

static bool
isModuleNameGood(j9object_t moduleName);

static UDATA
allowReadAccessToModule(J9VMThread *currentThread, J9Module *fromModule, J9Module *toModule);

static void
trcModulesAddReadsModule(J9VMThread *currentThread, jobject toModule, J9Module *j9FromMod, J9Module *j9ToMod);
#endif /*JAVA_SPEC_VERSION >= 11*/

/**
 * Initializes the VM-interface from the supplied JNIEnv.
 */
void
initializeVMI(void)
{
	PORT_ACCESS_FROM_JAVAVM(BFUjavaVM);
	jint result;

	/* Register this module with trace */
	UT_MODULE_LOADED(J9_UTINTERFACE_FROM_VM(BFUjavaVM));
	Trc_SC_VMInitStages_Event1(BFUjavaVM->mainThread);
	result = BFUjavaVM->internalVMFunctions->GetEnv(reinterpret_cast<JavaVM *>(BFUjavaVM),
	                                                reinterpret_cast<void **>(&g_VMI),
													SUNVMI_VERSION_1_1);
	if (result != JNI_OK) {
		j9tty_printf(PORTLIB, "FATAL ERROR: Could not obtain SUNVMI from VM.\n");
		exit(-1);
	}
}

jobject JNICALL
JVM_LatestUserDefinedLoader(JNIEnv *env)
{
	ENSURE_VMI();
	return g_VMI->JVM_LatestUserDefinedLoader(env);
}

#if JAVA_SPEC_VERSION >= 11
jobject JNICALL
JVM_GetCallerClass(JNIEnv *env)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetCallerClass(env);
}
#else /* JAVA_SPEC_VERSION >= 11 */
jobject JNICALL
JVM_GetCallerClass(JNIEnv *env, jint depth)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetCallerClass(env);
}
#endif /* JAVA_SPEC_VERSION >= 11 */

jobject JNICALL
JVM_NewInstanceFromConstructor(JNIEnv *env, jobject c, jobjectArray args)
{
	ENSURE_VMI();
	return g_VMI->JVM_NewInstanceFromConstructor(env, c, args);
}

jobject JNICALL
JVM_InvokeMethod(JNIEnv *env, jobject method, jobject obj, jobjectArray args)
{
	ENSURE_VMI();
	return g_VMI->JVM_InvokeMethod(env, method, obj, args);
}

jint JNICALL
JVM_GetClassAccessFlags(JNIEnv *env, jclass clazzRef)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetClassAccessFlags(env, clazzRef);
}

jobject JNICALL
JVM_GetClassContext(JNIEnv *env)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetClassContext(env);
}

void JNICALL
JVM_Halt(jint exitCode)
{
	ENSURE_VMI();
	g_VMI->JVM_Halt(exitCode);
}

void JNICALL
JVM_GCNoCompact(void)
{
	ENSURE_VMI();
	g_VMI->JVM_GCNoCompact();
}

void JNICALL
JVM_GC(void)
{
	ENSURE_VMI();
	g_VMI->JVM_GC();
}

/**
 * JVM_TotalMemory
 */
jlong JNICALL
JVM_TotalMemory(void)
{
	ENSURE_VMI();
	return g_VMI->JVM_TotalMemory();
}

jlong JNICALL
JVM_FreeMemory(void)
{
	ENSURE_VMI();
	return g_VMI->JVM_FreeMemory();
}

jobject JNICALL
JVM_GetSystemPackages(JNIEnv *env)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetSystemPackages(env);
}

/**
 * Returns the package information for the specified package name.  Package information is the directory or
 * jar file name from where the package was loaded (separator is to be '/' and for a directory the return string is
 * to end with a '/' character). If the package is not loaded then null is to be returned.
 *
 * @arg[in] env     - JNI environment.
 * @arg[in] pkgName -  A Java string for the name of a package. The package must be separated with '/' and optionally end with a '/' character.
 *
 * @return Package information as a string.
 *
 * @note In the current implementation, the separator is not guaranteed to be '/', not is a directory guaranteed to be
 * terminated with a slash. It is also unclear what the expected implementation is for UNC paths.
 *
 * @note see CMVC defects 81175 and 92979
 */
jstring JNICALL
JVM_GetSystemPackage(JNIEnv *env, jstring pkgName)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetSystemPackage(env, pkgName);
}

jobject JNICALL
JVM_AllocateNewObject(JNIEnv *env, jclass caller, jclass current, jclass init)
{
	ENSURE_VMI();
	return g_VMI->JVM_AllocateNewObject(env, caller, current, init);
}

jobject JNICALL
JVM_AllocateNewArray(JNIEnv *env, jclass caller, jclass current, jint length)
{
	ENSURE_VMI();
	return g_VMI->JVM_AllocateNewArray(env, caller, current, length);
}

jobject JNICALL
JVM_GetClassLoader(JNIEnv *env, jobject obj)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetClassLoader(env, obj);
}

void *JNICALL
JVM_GetThreadInterruptEvent(void)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetThreadInterruptEvent();
}

jlong JNICALL
JVM_MaxObjectInspectionAge(void)
{
	ENSURE_VMI();
	return g_VMI->JVM_MaxObjectInspectionAge();
}

jlong JNICALL
JVM_MaxMemory(void)
{
	ENSURE_VMI();
	return g_VMI->JVM_MaxMemory();
}

// end of vmi.c

static J9ThreadEnv *
getJ9ThreadEnv(JNIEnv *env)
{
	JavaVM *jniVM = nullptr;
	static J9ThreadEnv *threadEnv = nullptr;

	if (nullptr != threadEnv) {
		return threadEnv;
	}

	/* Get the thread functions */
	env->GetJavaVM(&jniVM);
	jniVM->GetEnv(reinterpret_cast<void **>(&threadEnv), J9THREAD_VERSION_1_1);
	return threadEnv;
}

/**
 * Copies the contents of <code>array1</code> starting at offset <code>start1</code>
 * into <code>array2</code> starting at offset <code>start2</code> for
 * <code>length</code> elements.
 *
 * @param		array1 		the array to copy out of
 * @param		start1 		the starting index in array1
 * @param		array2 		the array to copy into
 * @param		start2 		the starting index in array2
 * @param		length 		the number of elements in the array to copy
 */
void JNICALL
JVM_ArrayCopy(JNIEnv *env, jclass ignored, jobject src, jint src_pos, jobject dst, jint dst_pos, jint length)
{
	J9VMThread *currentThread = nullptr;
	J9JavaVM *vm = nullptr;
	J9InternalVMFunctions *vmFuncs = nullptr;

	Assert_SC_notNull(env);

	currentThread = reinterpret_cast<J9VMThread *>(env);
	vm = currentThread->javaVM;
	vmFuncs = vm->internalVMFunctions;

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if ((nullptr == src) || (nullptr == dst)) {
		vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, nullptr);
	} else {
		j9object_t srcArray = J9_JNI_UNWRAP_REFERENCE(src);
		j9object_t dstArray = J9_JNI_UNWRAP_REFERENCE(dst);

		J9ArrayClass *srcArrayClass = reinterpret_cast<J9ArrayClass *>(J9OBJECT_CLAZZ(currentThread, srcArray));
		J9ArrayClass *dstArrayClass = reinterpret_cast<J9ArrayClass *>(J9OBJECT_CLAZZ(currentThread, dstArray));

		if (J9CLASS_IS_ARRAY(srcArrayClass) && J9CLASS_IS_ARRAY(dstArrayClass)) {
			if ((src_pos < 0)
				|| (dst_pos < 0)
				|| (length < 0)
			    || (static_cast<jint>(J9INDEXABLEOBJECT_SIZE(currentThread, srcArray)) < (src_pos + length))
			    || (static_cast<jint>(J9INDEXABLEOBJECT_SIZE(currentThread, dstArray)) < (dst_pos + length))
			) {
				vmFuncs->setCurrentException(
				        currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYINDEXOUTOFBOUNDSEXCEPTION, nullptr);
			} else {
				J9Class *srcTypeOfArray = srcArrayClass->componentType;
				J9Class *dstTypeOfArray = dstArrayClass->componentType;

				int i = 0;
				if (J9ROMCLASS_IS_PRIMITIVE_TYPE(srcTypeOfArray->romClass)
				    && J9ROMCLASS_IS_PRIMITIVE_TYPE(dstTypeOfArray->romClass)
				) {
					if (srcTypeOfArray == dstTypeOfArray) {
						if (vm->longReflectClass == srcTypeOfArray) {
							if ((srcArray == dstArray)
							    && (((src_pos < dst_pos)
								&& (src_pos + length > dst_pos)))
							) {
								for (i = length - 1; i >= 0; i--) {
									J9JAVAARRAYOFLONG_STORE(
									        currentThread,
											dstArray,
											i + dst_pos,
									        J9JAVAARRAYOFLONG_LOAD(currentThread,
																   srcArray,
																   i + src_pos));
								}
							} else {
								for (i = 0; i < length; i++) {
									J9JAVAARRAYOFLONG_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFLONG_LOAD(currentThread,
									                               srcArray,
									                               i + src_pos));
								}
							}
						} else if (vm->booleanReflectClass == srcTypeOfArray) {
							if ((srcArray == dstArray)
							    && (((src_pos < dst_pos)
								&& (src_pos + length > dst_pos)))
							) {
								for (i = length - 1; i >= 0; i--) {
									J9JAVAARRAYOFBOOLEAN_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFBOOLEAN_LOAD(currentThread,
									                                  srcArray,
									                                  i + src_pos));
								}
							} else {
								for (i = 0; i < length; i++) {
									J9JAVAARRAYOFBOOLEAN_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFBOOLEAN_LOAD(currentThread,
									                                  srcArray,
									                                  i + src_pos));
								}
							}
						} else if (vm->byteReflectClass == srcTypeOfArray) {
							if ((srcArray == dstArray)
							    && (((src_pos < dst_pos)
								&& (src_pos + length > dst_pos)))
							) {
								for (i = length - 1; i >= 0; i--) {
									J9JAVAARRAYOFBYTE_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFBYTE_LOAD(currentThread,
									                               srcArray,
									                               i + src_pos));
								}
							} else {
								for (i = 0; i < length; i++) {
									J9JAVAARRAYOFBYTE_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFBYTE_LOAD(currentThread,
									                               srcArray,
									                               i + src_pos));
								}
							}
						} else if (vm->charReflectClass == srcTypeOfArray) {
							if ((srcArray == dstArray)
							    && (((src_pos < dst_pos) && (src_pos + length > dst_pos)))
							) {
								for (i = length - 1; i >= 0; i--) {
									J9JAVAARRAYOFCHAR_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFCHAR_LOAD(currentThread,
									                               srcArray,
									                               i + src_pos));
								}
							} else {
								for (i = 0; i < length; i++) {
									J9JAVAARRAYOFCHAR_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFCHAR_LOAD(currentThread,
									                               srcArray,
									                               i + src_pos));
								}
							}
						} else if (vm->shortReflectClass == srcTypeOfArray) {
							if ((srcArray == dstArray)
							    && (((src_pos < dst_pos) && (src_pos + length > dst_pos)))
							) {
								for (i = length - 1; i >= 0; i--) {
									J9JAVAARRAYOFSHORT_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFSHORT_LOAD(currentThread,
									                                srcArray,
									                                i + src_pos));
								}
							} else {
								for (i = 0; i < length; i++) {
									J9JAVAARRAYOFSHORT_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFSHORT_LOAD(currentThread,
									                                srcArray,
									                                i + src_pos));
								}
							}
						} else if (vm->intReflectClass == srcTypeOfArray) {
							if ((srcArray == dstArray)
							    && (((src_pos < dst_pos) && (src_pos + length > dst_pos)))
							) {
								for (i = length - 1; i >= 0; i--) {
									J9JAVAARRAYOFINT_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFINT_LOAD(currentThread,
									                              srcArray,
									                              i + src_pos));
								}
							} else {
								for (i = 0; i < length; i++) {
									J9JAVAARRAYOFINT_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFINT_LOAD(currentThread,
									                              srcArray,
									                              i + src_pos));
								}
							}
						} else if (vm->floatReflectClass == srcTypeOfArray) {
							if ((srcArray == dstArray)
							    && (((src_pos < dst_pos) && (src_pos + length > dst_pos)))
							) {
								for (i = length - 1; i >= 0; i--) {
									J9JAVAARRAYOFFLOAT_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFFLOAT_LOAD(currentThread,
									                                srcArray,
									                                i + src_pos));
								}
							} else {
								for (i = 0; i < length; i++) {
									J9JAVAARRAYOFFLOAT_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFFLOAT_LOAD(currentThread,
									                                srcArray,
									                                i + src_pos));
								}
							}
						} else if (vm->doubleReflectClass == srcTypeOfArray) {
							if ((srcArray == dstArray)
							    && (((src_pos < dst_pos) && (src_pos + length > dst_pos)))
							) {
								for (i = length - 1; i >= 0; i--) {
									J9JAVAARRAYOFDOUBLE_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFDOUBLE_LOAD(currentThread,
									                                 srcArray,
									                                 i + src_pos));
								}
							} else {
								for (i = 0; i < length; i++) {
									J9JAVAARRAYOFDOUBLE_STORE(
									        currentThread, dstArray, i + dst_pos,
									        J9JAVAARRAYOFDOUBLE_LOAD(currentThread,
									                                 srcArray,
									                                 i + src_pos));
								}
							}
						} else {
							vmFuncs->setCurrentException(
							        currentThread,
							        J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
							        nullptr);
						}
					} else {
						vmFuncs->setCurrentException(
						        currentThread,
								J9VMCONSTANTPOOL_JAVALANGARRAYSTOREEXCEPTION,
						        nullptr);
					}
				} else if (!J9ROMCLASS_IS_PRIMITIVE_TYPE(srcTypeOfArray->romClass)
				           && !J9ROMCLASS_IS_PRIMITIVE_TYPE(dstTypeOfArray->romClass)
				) {
					if (srcArray == dstArray) {
						if ((((src_pos < dst_pos) && (src_pos + length > dst_pos)))) {
							for (i = length - 1; i >= 0; i--) {
								J9JAVAARRAYOFOBJECT_STORE(
								        currentThread, dstArray, i + dst_pos,
								        J9JAVAARRAYOFOBJECT_LOAD(
								                currentThread, srcArray, i + src_pos));
							}
						} else {
							for (i = 0; i < length; i++) {
								J9JAVAARRAYOFOBJECT_STORE(
								        currentThread, dstArray, i + dst_pos,
								        J9JAVAARRAYOFOBJECT_LOAD(
								                currentThread, srcArray, i + src_pos));
							}
						}
					} else {
						j9object_t srcObject = nullptr;
						J9Class *srcObjectClass = nullptr;
						for (i = 0; i < length; i++) {
							srcObject = J9JAVAARRAYOFOBJECT_LOAD(currentThread, srcArray,
							                                     i + src_pos);
							if (nullptr == srcObject) {
								J9JAVAARRAYOFOBJECT_STORE(currentThread, dstArray,
								                          i + dst_pos, srcObject);
							} else {
								srcObjectClass =
								        J9OBJECT_CLAZZ(currentThread, srcObject);

								if (isSameOrSuperClassOf(dstTypeOfArray, srcObjectClass)) {
									J9JAVAARRAYOFOBJECT_STORE(currentThread,
									                          dstArray,
															  i + dst_pos,
									                          srcObject);
								} else {
									vmFuncs->setCurrentException(
									        currentThread,
									        J9VMCONSTANTPOOL_JAVALANGARRAYSTOREEXCEPTION,
									        nullptr);
									break;
								}
							}
						}
					}
				} else {
					vmFuncs->setCurrentException(
					        currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYSTOREEXCEPTION, nullptr);
				}
			}
		} else {
			vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYSTOREEXCEPTION, nullptr);
		}
	}
	vmFuncs->internalExitVMToJNI(currentThread);
}

jobject JNICALL
JVM_AssertionStatusDirectives(jint arg0, jint arg1)
{
	assert(!"JVM_AssertionStatusDirectives() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_Clone(jint arg0, jint arg1)
{
	assert(!"JVM_Clone() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_CompileClass(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_CompileClass() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_CompileClasses(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_CompileClasses() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_CompilerCommand(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_CompilerCommand() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_CountStackFrames(jint arg0, jint arg1)
{
	assert(!"JVM_CountStackFrames() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_CurrentThread(JNIEnv *env, jclass java_lang_Thread)
{
	J9VMThread *vmThread = reinterpret_cast<J9VMThread *>(env);
	if (nullptr == vmThread->threadObject) {
		return nullptr;
	}
	return reinterpret_cast<jobject>(&vmThread->threadObject);
}

jboolean JNICALL
JVM_DesiredAssertionStatus(JNIEnv *env, jobject arg1, jobject arg2)
{
	return JNI_FALSE;
}

jobject JNICALL
JVM_DisableCompiler(jint arg0, jint arg1)
{
	assert(!"JVM_DisableCompiler() stubbed!");
	return nullptr;
}

static jclass
java_lang_J9VMInternals(JNIEnv *env)
{
	static jclass cached = nullptr;
	jclass localRef = nullptr;

	if (nullptr != cached) {
		return cached;
	}

	localRef = env->FindClass("java/lang/J9VMInternals");
	assert(localRef != nullptr);

	cached = static_cast<jclass>(env->NewGlobalRef(localRef));
	if (cached == nullptr) {
		return nullptr;
	}

	env->DeleteLocalRef(localRef);
	assert(localRef != nullptr);
	return cached;
}

static jmethodID
java_lang_J9VMInternals_doPrivileged(JNIEnv *env)
{
	static jmethodID cached = nullptr;
	if (nullptr != cached) {
		return cached;
	}

	cached = env->GetStaticMethodID(java_lang_J9VMInternals(env), "doPrivileged",
	                                "(Ljava/security/PrivilegedAction;)Ljava/lang/Object;");
	assert(cached != nullptr);
	return cached;
}

static jmethodID
java_lang_J9VMInternals_doPrivilegedWithException(JNIEnv *env)
{
	static jmethodID cached = nullptr;
	if (nullptr != cached) {
		return cached;
	}

	cached = env->GetStaticMethodID(java_lang_J9VMInternals(env), "doPrivileged",
	                                "(Ljava/security/PrivilegedExceptionAction;)Ljava/lang/Object;");
	assert(cached != nullptr);
	return cached;
}

jobject JNICALL
JVM_DoPrivileged(JNIEnv *env,
                 jobject java_security_AccessController,
                 jobject action,
                 jboolean unknown,
                 jboolean isExceptionAction)
{
	PORT_ACCESS_FROM_ENV(env);
#if 0
	j9tty_printf(
			PORTLIB,
			"JVM_DoPrivileged(env=0x%p, accessController=0x%p, action=0x%p, arg3=0x%p, arg4=0x%p\n",
			env, java_security_AccessController, action, arg3, arg4);
#endif

	jmethodID methodID = (JNI_TRUE == isExceptionAction) ? java_lang_J9VMInternals_doPrivilegedWithException(env)
	                                                     : java_lang_J9VMInternals_doPrivileged(env);

	return env->CallStaticObjectMethod(java_lang_J9VMInternals(env), methodID, action);
}

jobject JNICALL
JVM_EnableCompiler(jint arg0, jint arg1)
{
	assert(!"JVM_EnableCompiler() stubbed!");
	return nullptr;
}

void JNICALL
JVM_FillInStackTrace(JNIEnv *env, jobject throwable)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *javaVM = currentThread->javaVM;
	J9InternalVMFunctions *vmfns = javaVM->internalVMFunctions;
	j9object_t unwrappedThrowable = nullptr;

	vmfns->internalEnterVMFromJNI(currentThread);
	unwrappedThrowable = J9_JNI_UNWRAP_REFERENCE(throwable);
	if ((0 == (javaVM->runtimeFlags & J9_RUNTIME_OMIT_STACK_TRACES)) &&
	    /* If the disableWritableStackTrace field is true, do not create the stack trace. */
	    !J9VMJAVALANGTHROWABLE_DISABLEWRITABLESTACKTRACE(currentThread, unwrappedThrowable)) {
		UDATA flags = J9_STACKWALK_CACHE_PCS | J9_STACKWALK_WALK_TRANSLATE_PC | J9_STACKWALK_VISIBLE_ONLY
		        | J9_STACKWALK_INCLUDE_NATIVES | J9_STACKWALK_SKIP_INLINES;
		J9StackWalkState *walkState = currentThread->stackWalkState;
		j9object_t result = static_cast<j9object_t>(J9VMJAVALANGTHROWABLE_WALKBACK(currentThread, unwrappedThrowable));
		UDATA rc = 0;
		UDATA i = 0;
		UDATA framesWalked = 0;

		/* Do not hide exception frames if fillInStackTrace is called on an exception which already has a stack trace.  In the out of memory case,
		 * there is a bit indicating that we should explicitly override this behaviour, since we've precached the stack trace array. */
		if ((nullptr == result)
		    || (J9_PRIVATE_FLAGS_FILL_EXISTING_TRACE
		        == (currentThread->privateFlags & J9_PRIVATE_FLAGS_FILL_EXISTING_TRACE))) {
			flags |= J9_STACKWALK_HIDE_EXCEPTION_FRAMES;
			walkState->restartException = unwrappedThrowable;
		}
		walkState->skipCount = 1; /* skip the INL frame -- TODO revisit this */
#if JAVA_SPEC_VERSION >= 15
		{
			J9Class *receiverClass = J9OBJECT_CLAZZ(currentThread, unwrappedThrowable);
			if (J9VMJAVALANGNULLPOINTEREXCEPTION_OR_NULL(javaVM) == receiverClass) {
				walkState->skipCount =
				        2; /* skip the INL & NullPointerException.fillInStackTrace() frames */
			}
		}
#endif /* JAVA_SPEC_VERSION >= 15 */
		walkState->walkThread = currentThread;
		walkState->flags = flags;

		rc = javaVM->walkStackFrames(currentThread, walkState);

		if (J9_STACKWALK_RC_NONE != rc) {
			/* Avoid infinite recursion if already throwing OOM. */
			if (J9_PRIVATE_FLAGS_OUT_OF_MEMORY
			    == (currentThread->privateFlags & J9_PRIVATE_FLAGS_OUT_OF_MEMORY)) {
				goto setThrowableSlots;
			}
			vmfns->setNativeOutOfMemoryError(
			        currentThread,
			        J9NLS_JCL_FAILED_TO_CREATE_STACK_TRACE); /* TODO replace with local NLS message */
			goto done;
		}
		framesWalked = walkState->framesWalked;

		/* If there is no stack trace in the exception, or we are not in the out of memory case, allocate a new stack trace. */
		if ((nullptr == result) || (0 == (currentThread->privateFlags & J9_PRIVATE_FLAGS_FILL_EXISTING_TRACE))) {
#ifdef J9VM_ENV_DATA64
			J9Class *arrayClass = javaVM->longArrayClass;
#else
			J9Class *arrayClass = javaVM->intArrayClass;
#endif
			result = javaVM->memoryManagerFunctions->J9AllocateIndexableObject(
			        currentThread, arrayClass, static_cast<U_32>(framesWalked),
			        J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
			if (nullptr == result) {
				vmfns->setHeapOutOfMemoryError(currentThread);
				goto done;
			}
			/* Reload after allocation */
			unwrappedThrowable = J9_JNI_UNWRAP_REFERENCE(throwable);
		} else {
			UDATA maxSize = J9INDEXABLEOBJECT_SIZE(currentThread, result);
			if (framesWalked > maxSize) {
				framesWalked = maxSize;
			}
		}

		for (i = 0; i < framesWalked; ++i) {
			J9JAVAARRAYOFUDATA_STORE(currentThread, result, i, walkState->cache[i]);
		}

		vmfns->freeStackWalkCaches(currentThread, walkState);

	setThrowableSlots:
		J9VMJAVALANGTHROWABLE_SET_WALKBACK(currentThread, unwrappedThrowable, result);
		J9VMJAVALANGTHROWABLE_SET_STACKTRACE(currentThread, unwrappedThrowable, nullptr);
	}
done:
	vmfns->internalExitVMToJNI(currentThread);
}

/**
 * Find the specified class in given class loader
 *
 * @param env
 * @param className    null-terminated class name string.
 * @param init         initialize the class when set
 * @param classLoader  classloader of the class
 * @param throwError   set to true in order to throw errors
 * @return Assumed to be a jclass.
 *
 * Note: this call is implemented from info provided via CMVC 154874.
 */
jobject JNICALL
JVM_FindClassFromClassLoader(JNIEnv *env, char *className, jboolean init, jobject classLoader, jboolean throwError)
{
	ENSURE_VMI();
	return g_VMI->JVM_FindClassFromClassLoader(env, className, init, classLoader, throwError);
}

/**
 * Find the specified class using boot class loader
 *
 * @param env
 * @param className    null-terminated class name string.
 * @return jclass or nullptr on error.
 *
 * Note: this call is implemented from info provided via CMVC 156938.
 */
jobject JNICALL
JVM_FindClassFromBootLoader(JNIEnv *env, char *className)
{
	return JVM_FindClassFromClassLoader(env, className, JNI_TRUE, nullptr, JNI_FALSE);
}

jobject JNICALL
JVM_FindLoadedClass(JNIEnv *env, jobject classLoader, jobject className)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9ClassLoader *vmClassLoader;
	J9Class *loadedClass = nullptr;

	vm->internalVMFunctions->internalEnterVMFromJNI(currentThread);

	if (nullptr == className) {
		goto done;
	}

	vmClassLoader = J9VMJAVALANGCLASSLOADER_VMREF(currentThread, J9_JNI_UNWRAP_REFERENCE(classLoader));
	if (nullptr == vmClassLoader) {
		goto done;
	}

	loadedClass = vm->internalVMFunctions->internalFindClassString(
	        currentThread, nullptr, J9_JNI_UNWRAP_REFERENCE(className), vmClassLoader, J9_FINDCLASS_FLAG_EXISTING_ONLY,
	        CLASSNAME_INVALID);
done:
	vm->internalVMFunctions->internalExitVMToJNI(currentThread);

	if (nullptr == loadedClass) {
		return nullptr;
	}

	return reinterpret_cast<jobject>(&loadedClass->classObject);
}

jobject JNICALL
JVM_FindPrimitiveClass(JNIEnv *env, char *name)
{
	J9JavaVM *vm = (reinterpret_cast<J9VMThread *>(env))->javaVM;

	/* code inspired by reflecthelp.c */
	if (0 == strcmp(name, "int")) {
		return reinterpret_cast<jobject>(&vm->intReflectClass->classObject);
	}
	if (0 == strcmp(name, "boolean")) {
		return reinterpret_cast<jobject>(&vm->booleanReflectClass->classObject);
	}
	if (0 == strcmp(name, "long")) {
		return reinterpret_cast<jobject>(&vm->longReflectClass->classObject);
	}
	if (0 == strcmp(name, "double")) {
		return reinterpret_cast<jobject>(&vm->doubleReflectClass->classObject);
	}
	if (0 == strcmp(name, "float")) {
		return reinterpret_cast<jobject>(&vm->floatReflectClass->classObject);
	}
	if (0 == strcmp(name, "char")) {
		return reinterpret_cast<jobject>(&vm->charReflectClass->classObject);
	}
	if (0 == strcmp(name, "byte")) {
		return reinterpret_cast<jobject>(&vm->byteReflectClass->classObject);
	}
	if (0 == strcmp(name, "short")) {
		return reinterpret_cast<jobject>(&vm->shortReflectClass->classObject);
	}
	if (0 == strcmp(name, "void")) {
		return reinterpret_cast<jobject>(&vm->voidReflectClass->classObject);
	}

	assert(!"JVM_FindPrimitiveClass() stubbed!");
	return nullptr;
}

/**
 * Get the Array element at the index
 * This function may lock, gc or throw exception.
 * @param array The array
 * @param index The index
 * @return the element at the index
 */
jobject JNICALL
JVM_GetArrayElement(JNIEnv *env, jobject array, jint index)
{
	J9VMThread *currentThread = nullptr;
	J9JavaVM *vm = nullptr;
	J9InternalVMFunctions *vmFuncs = nullptr;
	jobject elementJNIRef = nullptr;

	Assert_SC_notNull(env);

	currentThread = reinterpret_cast<J9VMThread *>(env);
	vm = currentThread->javaVM;
	vmFuncs = vm->internalVMFunctions;

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if (nullptr == array) {
		vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, nullptr);
	} else {
		j9object_t j9array = J9_JNI_UNWRAP_REFERENCE(array);
		J9ArrayClass *arrayClass = reinterpret_cast<J9ArrayClass *>(J9OBJECT_CLAZZ(currentThread, j9array));
		J9Class *typeOfArray = arrayClass->componentType;

		if (J9CLASS_IS_ARRAY(arrayClass)) {
			if ((index < 0) || (static_cast<jint>(J9INDEXABLEOBJECT_SIZE(currentThread, j9array)) <= index)) {
				vmFuncs->setCurrentException(
				        currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYINDEXOUTOFBOUNDSEXCEPTION, nullptr);
			} else {
				if (J9ROMCLASS_IS_PRIMITIVE_TYPE(typeOfArray->romClass)) {
					j9object_t primitiveElement = nullptr;
					J9MemoryManagerFunctions *memManagerFuncs = vm->memoryManagerFunctions;
					bool illegalArgSeen = FALSE;

					if (vm->longReflectClass == typeOfArray) {
						primitiveElement = memManagerFuncs->J9AllocateObject(
						        currentThread, J9VMJAVALANGLONG_OR_NULL(vm),
						        J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
						if (nullptr != primitiveElement) {
							jlong val =
							        J9JAVAARRAYOFLONG_LOAD(currentThread, j9array, index);
							J9VMJAVALANGLONG_SET_VALUE(currentThread, primitiveElement,
							                           val);
							elementJNIRef = vmFuncs->j9jni_createLocalRef(
							        reinterpret_cast<JNIEnv *>(currentThread), primitiveElement);
						}
					} else if (vm->booleanReflectClass == typeOfArray) {
						primitiveElement = memManagerFuncs->J9AllocateObject(
						        currentThread, J9VMJAVALANGBOOLEAN_OR_NULL(vm),
						        J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
						if (nullptr != primitiveElement) {
							jboolean val = J9JAVAARRAYOFBOOLEAN_LOAD(currentThread, j9array,
							                                         index);
							J9VMJAVALANGBOOLEAN_SET_VALUE(currentThread, primitiveElement,
							                              val);
							elementJNIRef = vmFuncs->j9jni_createLocalRef(
							        reinterpret_cast<JNIEnv *>(currentThread), primitiveElement);
						}
					} else if (vm->byteReflectClass == typeOfArray) {
						primitiveElement = memManagerFuncs->J9AllocateObject(
						        currentThread, J9VMJAVALANGBYTE_OR_NULL(vm),
						        J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
						if (nullptr != primitiveElement) {
							jbyte val =
							        J9JAVAARRAYOFBYTE_LOAD(currentThread, j9array, index);
							J9VMJAVALANGBYTE_SET_VALUE(currentThread, primitiveElement,
							                           val);
							elementJNIRef = vmFuncs->j9jni_createLocalRef(
							        reinterpret_cast<JNIEnv *>(currentThread), primitiveElement);
						}
					} else if (vm->charReflectClass == typeOfArray) {
						primitiveElement = memManagerFuncs->J9AllocateObject(
						        currentThread, J9VMJAVALANGCHARACTER_OR_NULL(vm),
						        J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
						if (nullptr != primitiveElement) {
							jchar val =
							        J9JAVAARRAYOFCHAR_LOAD(currentThread, j9array, index);
							J9VMJAVALANGCHARACTER_SET_VALUE(currentThread, primitiveElement,
							                                val);
							elementJNIRef = vmFuncs->j9jni_createLocalRef(
							        reinterpret_cast<JNIEnv *>(currentThread), primitiveElement);
						}
					} else if (vm->shortReflectClass == typeOfArray) {
						primitiveElement = memManagerFuncs->J9AllocateObject(
						        currentThread, J9VMJAVALANGSHORT_OR_NULL(vm),
						        J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
						if (nullptr != primitiveElement) {
							jshort val =
							        J9JAVAARRAYOFSHORT_LOAD(currentThread, j9array, index);
							J9VMJAVALANGSHORT_SET_VALUE(currentThread, primitiveElement,
							                            val);
							elementJNIRef = vmFuncs->j9jni_createLocalRef(
							        reinterpret_cast<JNIEnv *>(currentThread), primitiveElement);
						}
					} else if (vm->intReflectClass == typeOfArray) {
						primitiveElement = memManagerFuncs->J9AllocateObject(
						        currentThread, J9VMJAVALANGINTEGER_OR_NULL(vm),
						        J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
						if (nullptr != primitiveElement) {
							jint val = J9JAVAARRAYOFINT_LOAD(currentThread, j9array, index);
							J9VMJAVALANGINTEGER_SET_VALUE(currentThread, primitiveElement,
							                              val);
							elementJNIRef = vmFuncs->j9jni_createLocalRef(
							        reinterpret_cast<JNIEnv *>(currentThread), primitiveElement);
						}
					} else if (vm->floatReflectClass == typeOfArray) {
						primitiveElement = memManagerFuncs->J9AllocateObject(
						        currentThread, J9VMJAVALANGFLOAT_OR_NULL(vm),
						        J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
						if (nullptr != primitiveElement) {
							U_32 val =
							        J9JAVAARRAYOFFLOAT_LOAD(currentThread, j9array, index);
							J9VMJAVALANGFLOAT_SET_VALUE(currentThread, primitiveElement,
							                            val);
							elementJNIRef = vmFuncs->j9jni_createLocalRef(
							        reinterpret_cast<JNIEnv *>(currentThread), primitiveElement);
						}
					} else if (vm->doubleReflectClass == typeOfArray) {
						primitiveElement = memManagerFuncs->J9AllocateObject(
						        currentThread, J9VMJAVALANGDOUBLE_OR_NULL(vm),
						        J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
						if (nullptr != primitiveElement) {
							U_64 val =
							        J9JAVAARRAYOFDOUBLE_LOAD(currentThread, j9array, index);
							J9VMJAVALANGDOUBLE_SET_VALUE(currentThread, primitiveElement,
							                             val);
							elementJNIRef = vmFuncs->j9jni_createLocalRef(
							        reinterpret_cast<JNIEnv *>(currentThread), primitiveElement);
						}
					} else {
						vmFuncs->setCurrentException(
						        currentThread,
						        J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION, nullptr);
						illegalArgSeen = TRUE;
					}
					if (!illegalArgSeen) {
						if (nullptr == primitiveElement) {
							vmFuncs->setHeapOutOfMemoryError(currentThread);
						} else if (nullptr == elementJNIRef) {
							vmFuncs->setNativeOutOfMemoryError(currentThread,
							                                   J9NLS_VM_NATIVE_OOM);
						}
					}
				} else {
					j9object_t j9arrayElement =
					        J9JAVAARRAYOFOBJECT_LOAD(currentThread, j9array, index);
					elementJNIRef =
					        vmFuncs->j9jni_createLocalRef(reinterpret_cast<JNIEnv *>(currentThread), j9arrayElement);

					if ((nullptr == elementJNIRef) && (nullptr != j9arrayElement)) {
						vmFuncs->setNativeOutOfMemoryError(currentThread, J9NLS_VM_NATIVE_OOM);
					}
				}
			}
		} else {
			vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
			                             nullptr);
		}
	}
	vmFuncs->internalExitVMToJNI(currentThread);

	return elementJNIRef;
}

/**
 * Get the array length.
 * This function may lock, gc or throw exception.
 * @param array The array
 * @return the array length
 */
jint JNICALL
JVM_GetArrayLength(JNIEnv *env, jobject array)
{
	J9VMThread *currentThread = nullptr;
	J9JavaVM *vm = nullptr;
	J9InternalVMFunctions *vmFuncs = nullptr;
	jsize arrayLength = 0;

	Assert_SC_notNull(env);

	currentThread = reinterpret_cast<J9VMThread *>(env);
	vm = currentThread->javaVM;
	vmFuncs = vm->internalVMFunctions;

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if (nullptr == array) {
		vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, nullptr);
	} else {
		J9Class *ramClass = nullptr;
		j9object_t j9array = nullptr;

		j9array = J9_JNI_UNWRAP_REFERENCE(array);
		ramClass = J9OBJECT_CLAZZ(currentThread, j9array);

		if (J9CLASS_IS_ARRAY(ramClass)) {
			arrayLength = J9INDEXABLEOBJECT_SIZE(currentThread, j9array);
		} else {
			vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
			                             nullptr);
		}
	}

	vmFuncs->internalExitVMToJNI(currentThread);

	return arrayLength;
}

J9Class *
java_lang_Class_vmRef(JNIEnv *env, jobject clazz)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9Class *ramClass = nullptr;

	vm->internalVMFunctions->internalEnterVMFromJNI(currentThread);
	ramClass = J9VMJAVALANGCLASS_VMREF(currentThread, J9_JNI_UNWRAP_REFERENCE(clazz));
	vm->internalVMFunctions->internalExitVMToJNI(currentThread);

	return ramClass;
}

/**
 * Helper function to convert a J9UTF8* to a null-terminated C string.
 */
static char *
utf8_to_cstring(JNIEnv *env, J9UTF8 *utf)
{
	PORT_ACCESS_FROM_ENV(env);
	char *cstring = reinterpret_cast<char *>(j9mem_allocate_memory(J9UTF8_LENGTH(utf) + 1, OMRMEM_CATEGORY_VM));
	if (nullptr != cstring) {
		memcpy(cstring, J9UTF8_DATA(utf), J9UTF8_LENGTH(utf));
		cstring[J9UTF8_LENGTH(utf)] = 0;
	}
	return cstring;
}

/**
 * Helper function to convert a J9UTF8* to a java/lang/String.
 */
static jobject
utf8_to_java_lang_String(JNIEnv *env, J9UTF8 *utf)
{
	PORT_ACCESS_FROM_ENV(env);
	char *cstring = utf8_to_cstring(env, utf);
	jobject jlString = env->NewStringUTF(cstring);
	if (nullptr != cstring) {
		j9mem_free_memory(cstring);
	}
	return jlString;
}

jobject JNICALL
JVM_GetClassDeclaredConstructors(JNIEnv *env, jclass clazz, jboolean unknown)
{
	J9Class *ramClass = nullptr;
	J9ROMClass *romClass = nullptr;
	jclass jlrConstructor = nullptr;
	jobjectArray result = nullptr;
	const char *eyecatcher = "<init>";
	U_16 initLength = 6;
	jsize size = 0;
	PORT_ACCESS_FROM_ENV(env);

	ramClass = java_lang_Class_vmRef(env, clazz);
	romClass = ramClass->romClass;

	/* Primitives/Arrays don't have fields. */
	if (J9ROMCLASS_IS_PRIMITIVE_OR_ARRAY(romClass) || J9ROMCLASS_IS_INTERFACE(romClass)) {
		size = 0;
	} else {
		U_32 romMethodCount = romClass->romMethodCount;
		J9Method *method = ramClass->ramMethods;

		while (romMethodCount-- != 0) {
			J9ROMMethod *romMethod = J9_ROM_METHOD_FROM_RAM_METHOD(method++);
			J9UTF8 *nameUTF = J9ROMMETHOD_NAME(romMethod);

			if (J9UTF8_DATA_EQUALS(J9UTF8_DATA(nameUTF), J9UTF8_LENGTH(nameUTF), eyecatcher, 6)) {
				size++;
			}
		}
	}

	/* Look up the field class */
	jlrConstructor = env->FindClass("java/lang/reflect/Constructor");
	if (nullptr == jlrConstructor) {
		return nullptr;
	}

	/* Create the result array */
	result = env->NewObjectArray(size, jlrConstructor, nullptr);
	if (nullptr == result) {
		return nullptr;
	}

	/* Now walk and fill in the contents */
	if (size != 0) {
		U_32 romMethodCount = romClass->romMethodCount;
		J9Method *method = ramClass->ramMethods;
		jsize index = 0;

		while (romMethodCount-- != 0) {
			J9ROMMethod *romMethod = J9_ROM_METHOD_FROM_RAM_METHOD(method++);
			J9UTF8 *nameUTF = J9ROMMETHOD_NAME(romMethod);

			if (J9UTF8_DATA_EQUALS(J9UTF8_DATA(nameUTF), J9UTF8_LENGTH(nameUTF), eyecatcher, 6)) {
				J9UTF8 *signatureUTF = J9ROMMETHOD_SIGNATURE(romMethod);
				char *name = utf8_to_cstring(env, nameUTF);
				char *signature = utf8_to_cstring(env, signatureUTF);
				jmethodID methodID = env->GetMethodID(static_cast<jclass>(clazz), name, signature);
				jobject reflectedMethod = nullptr;

				assert(methodID != nullptr);
				if (nullptr != name) {
					j9mem_free_memory(name);
				}
				if (nullptr != signature) {
					j9mem_free_memory(signature);
				}

				reflectedMethod =
				        env->ToReflectedMethod(static_cast<jclass>(clazz), methodID, JNI_FALSE);
				assert(reflectedMethod != nullptr);
				env->SetObjectArrayElement(result, index++, reflectedMethod);
			}
		}
	}

	return result;
}

jobject JNICALL
JVM_GetClassDeclaredFields(JNIEnv *env, jobject clazz, jint arg2)
{
	J9Class *ramClass = nullptr;
	J9ROMClass *romClass = nullptr;
	jclass jlrField = nullptr;
	jsize size = 0;
	jobjectArray result = nullptr;
	J9ROMFieldShape *field = nullptr;
	J9ROMFieldWalkState walkState;
	jsize index = 0;
	PORT_ACCESS_FROM_ENV(env);

	ramClass = java_lang_Class_vmRef(env, clazz);
	romClass = ramClass->romClass;

	/* Primitives/Arrays don't have fields. */
	if (J9ROMCLASS_IS_PRIMITIVE_OR_ARRAY(romClass)) {
		size = 0;
	} else {
		size = romClass->romFieldCount;
	}

	/* Look up the field class */
	jlrField = env->FindClass("java/lang/reflect/Field");
	if (nullptr == jlrField) {
		return nullptr;
	}

	/* Create the result array */
	result = env->NewObjectArray(size, jlrField, nullptr);
	if (nullptr == result) {
		return nullptr;
	}

	/* Iterate through the fields */
	field = romFieldsStartDo(romClass, &walkState);
	while (nullptr != field) {
		U_32 modifiers = field->modifiers;
		jfieldID fieldID = 0;
		jobject reflectedField = nullptr;
		jboolean isStatic = JNI_FALSE;
		J9UTF8 *nameUTF = J9ROMFIELDSHAPE_NAME(field);
		J9UTF8 *signatureUTF = J9ROMFIELDSHAPE_SIGNATURE(field);
		char *name = utf8_to_cstring(env, nameUTF);
		char *signature = utf8_to_cstring(env, signatureUTF);

		if (J9_ARE_ANY_BITS_SET(modifiers, J9AccStatic)) {
			fieldID =
			        env->GetStaticFieldID(static_cast<jclass>(static_cast<jclass>(clazz)), name, signature);
			isStatic = JNI_TRUE;
		} else {
			fieldID = env->GetFieldID(static_cast<jclass>(static_cast<jclass>(clazz)), name, signature);
			isStatic = JNI_FALSE;
		}

		if (nullptr != name) {
			j9mem_free_memory(name);
		}
		if (nullptr != signature) {
			j9mem_free_memory(signature);
		}

		assert(fieldID != nullptr);
		reflectedField = env->ToReflectedField(static_cast<jclass>(clazz), fieldID, isStatic);
		assert(reflectedField != nullptr);
		env->SetObjectArrayElement(result, index++, reflectedField);
		field = romFieldsNextDo(&walkState);
	}

	return result;
}

jobject JNICALL
JVM_GetClassDeclaredMethods(JNIEnv *env, jobject clazz, jboolean unknown)
{
	J9Class *ramClass = nullptr;
	J9ROMClass *romClass = nullptr;
	jclass jlrMethod = nullptr;
	jobjectArray result = nullptr;
	const char *eyecatcher = "<init>";
	U_16 initLength = 6;
	jsize size = 0;
	PORT_ACCESS_FROM_ENV(env);

	ramClass = java_lang_Class_vmRef(env, clazz);
	romClass = ramClass->romClass;

	/* Primitives/Arrays don't have fields. */
	if (J9ROMCLASS_IS_PRIMITIVE_OR_ARRAY(romClass) || J9ROMCLASS_IS_INTERFACE(romClass)) {
		size = 0;
	} else {
		U_32 romMethodCount = romClass->romMethodCount;
		J9Method *method = ramClass->ramMethods;

		while (romMethodCount-- != 0) {
			J9ROMMethod *romMethod = J9_ROM_METHOD_FROM_RAM_METHOD(method++);
			J9UTF8 *nameUTF = J9ROMMETHOD_NAME(romMethod);

			if (!J9UTF8_DATA_EQUALS(J9UTF8_DATA(nameUTF), J9UTF8_LENGTH(nameUTF), eyecatcher, 6)) {
				size++;
			}
		}
	}

	/* Look up the field class */
	jlrMethod = env->FindClass("java/lang/reflect/Method");
	if (nullptr == jlrMethod) {
		return nullptr;
	}

	/* Create the result array */
	result = env->NewObjectArray(size, jlrMethod, nullptr);
	if (nullptr == result) {
		return nullptr;
	}

	/* Now walk and fill in the contents */
	if (size != 0) {
		U_32 romMethodCount = romClass->romMethodCount;
		J9Method *method = ramClass->ramMethods;
		jsize index = 0;

		while (romMethodCount-- != 0) {
			J9ROMMethod *romMethod = J9_ROM_METHOD_FROM_RAM_METHOD(method++);
			J9UTF8 *nameUTF = J9ROMMETHOD_NAME(romMethod);

			if (!J9UTF8_DATA_EQUALS(J9UTF8_DATA(nameUTF), J9UTF8_LENGTH(nameUTF), eyecatcher, 6)) {
				J9UTF8 *signatureUTF = J9ROMMETHOD_SIGNATURE(romMethod);
				char *name = utf8_to_cstring(env, nameUTF);
				char *signature = utf8_to_cstring(env, signatureUTF);
				U_32 modifiers = romMethod->modifiers;
				jmethodID methodID = nullptr;
				jobject reflectedMethod = nullptr;
				jboolean isStatic = JNI_FALSE;

				if (J9_ARE_ANY_BITS_SET(modifiers, J9AccStatic)) {
					methodID = env->GetStaticMethodID(static_cast<jclass>(clazz), name, signature);
					isStatic = JNI_TRUE;
				} else {
					methodID = env->GetMethodID(static_cast<jclass>(clazz), name, signature);
					isStatic = JNI_FALSE;
				}

				assert(methodID != nullptr);
				if (nullptr != name) {
					j9mem_free_memory(name);
				}
				if (nullptr != signature) {
					j9mem_free_memory(signature);
				}

				reflectedMethod =
				        env->ToReflectedMethod(static_cast<jclass>(clazz), methodID, isStatic);
				assert(reflectedMethod != nullptr);
				env->SetObjectArrayElement(result, index++, reflectedMethod);
			}
		}
	}

	return result;
}

jobject JNICALL
JVM_GetClassInterfaces(jint arg0, jint arg1)
{
	assert(!"JVM_GetClassInterfaces() stubbed!");
	return nullptr;
}

jint JNICALL
JVM_GetClassModifiers(JNIEnv *env, jclass clazz)
{
	J9Class *ramClass = java_lang_Class_vmRef(env, clazz);
	J9ROMClass *romClass = ramClass->romClass;

	if (J9ROMCLASS_IS_ARRAY(romClass)) {
		J9ArrayClass *arrayClass = reinterpret_cast<J9ArrayClass *>(ramClass);
		jint result = 0;
		J9ROMClass *leafRomClass = arrayClass->leafComponentType->romClass;
		if (J9_ARE_ALL_BITS_SET(leafRomClass->extraModifiers, J9AccClassInnerClass)) {
			result = leafRomClass->memberAccessFlags;
		} else {
			result = leafRomClass->modifiers;
		}

		result |= (J9AccAbstract | J9AccFinal);
		return result;
	} else {
		if (J9_ARE_ALL_BITS_SET(romClass->extraModifiers, J9AccClassInnerClass)) {
			return romClass->memberAccessFlags;
		} else {
			return romClass->modifiers;
		}
	}
}

jobject JNICALL
JVM_GetClassSigners(jint arg0, jint arg1)
{
	assert(!"JVM_GetClassSigners() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetComponentType(JNIEnv *env, jclass clazz)
{
	J9Class *ramClass = java_lang_Class_vmRef(env, clazz);
	J9ROMClass *romClass = ramClass->romClass;

	if (J9ROMCLASS_IS_ARRAY(romClass)) {
		J9ArrayClass *arrayClass = reinterpret_cast<J9ArrayClass *>(ramClass);
		return (jobject) & (arrayClass->leafComponentType->classObject);
	}
	return nullptr;
}

jobject JNICALL
JVM_GetDeclaredClasses(jint arg0, jint arg1)
{
	assert(!"JVM_GetDeclaredClasses() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetDeclaringClass(jint arg0, jint arg1)
{
	assert(!"JVM_GetDeclaringClass() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetInheritedAccessControlContext(jint arg0, jint arg1)
{
	assert(!"JVM_GetInheritedAccessControlContext() stubbed!");
	return nullptr;
}

/**
 * Get the primitive array element at index.
 * This function may lock, gc or throw exception.
 * @param array The array
 * @param index The index
 * @param wCode the native symbol code
 * @return a union of primitive value
 */
jvalue JNICALL
JVM_GetPrimitiveArrayElement(JNIEnv *env, jobject array, jint index, jint wCode)
{
	J9VMThread *currentThread = nullptr;
	J9JavaVM *vm = nullptr;
	J9InternalVMFunctions *vmFuncs = nullptr;
	jvalue value;

	Assert_SC_notNull(env);

	currentThread = reinterpret_cast<J9VMThread *>(env);
	vm = currentThread->javaVM;
	vmFuncs = vm->internalVMFunctions;
	value.j = 0;

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if (nullptr == array) {
		vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, nullptr);
	} else {
		j9object_t j9array = J9_JNI_UNWRAP_REFERENCE(array);
		J9ArrayClass *arrayClass = reinterpret_cast<J9ArrayClass *>(J9OBJECT_CLAZZ(currentThread, j9array));
		J9Class *typeOfArray = reinterpret_cast<J9Class *>(arrayClass->componentType);

		if (J9CLASS_IS_ARRAY(arrayClass) && J9ROMCLASS_IS_PRIMITIVE_TYPE(typeOfArray->romClass)) {
			if ((index < 0) || (static_cast<jint>(J9INDEXABLEOBJECT_SIZE(currentThread, j9array)) <= index)) {
				vmFuncs->setCurrentException(
				        currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYINDEXOUTOFBOUNDSEXCEPTION, nullptr);
			} else {
				bool invalidArgument = FALSE;

				if (vm->booleanReflectClass == typeOfArray) {
					if (POK_BOOLEAN == wCode) {
						value.z = J9JAVAARRAYOFBOOLEAN_LOAD(currentThread, j9array, index);
					} else {
						invalidArgument = TRUE;
					}
				} else if (vm->charReflectClass == typeOfArray) {
					switch (wCode) {
					case POK_CHAR:
						value.c = J9JAVAARRAYOFCHAR_LOAD(currentThread, j9array, index);
						break;
					case POK_FLOAT:
						value.f = static_cast<jfloat>(J9JAVAARRAYOFCHAR_LOAD(currentThread, j9array, index));
						break;
					case POK_DOUBLE:
						value.d = static_cast<jdouble>(J9JAVAARRAYOFCHAR_LOAD(currentThread, j9array, index));
						break;
					case POK_INT:
						value.i = J9JAVAARRAYOFCHAR_LOAD(currentThread, j9array, index);
						break;
					case POK_LONG:
						value.j = J9JAVAARRAYOFCHAR_LOAD(currentThread, j9array, index);
						break;
					default:
						invalidArgument = TRUE;
						break;
					}
				} else if (vm->floatReflectClass == typeOfArray) {
					jfloat val = 0;
					switch (wCode) {
					case POK_FLOAT:
						*reinterpret_cast<U_32 *>(&value.f) =
						        J9JAVAARRAYOFFLOAT_LOAD(currentThread, j9array, index);
						break;
					case POK_DOUBLE:
						*reinterpret_cast<U_32 *>(&val) = J9JAVAARRAYOFFLOAT_LOAD(currentThread, j9array, index);
						value.d = static_cast<jdouble>(val);
						break;
					default:
					invalidArgument = TRUE;
					break;
					}
				} else if (vm->doubleReflectClass == typeOfArray) {
					if (POK_DOUBLE == wCode) {
						*reinterpret_cast<U_64 *>(&value.d) =
						        J9JAVAARRAYOFDOUBLE_LOAD(currentThread, j9array, index);
					} else {
						invalidArgument = TRUE;
					}
				} else if (vm->byteReflectClass == typeOfArray) {
					switch (wCode) {
					case POK_FLOAT:
						value.f = static_cast<jfloat>(J9JAVAARRAYOFBYTE_LOAD(currentThread, j9array, index));
						break;
					case POK_DOUBLE:
						value.d = static_cast<jdouble>(J9JAVAARRAYOFBYTE_LOAD(currentThread, j9array, index));
						break;
					case POK_BYTE:
						value.b = J9JAVAARRAYOFBYTE_LOAD(currentThread, j9array, index);
						break;
					case POK_SHORT:
						value.s = J9JAVAARRAYOFBYTE_LOAD(currentThread, j9array, index);
						break;
					case POK_INT:
						value.i = J9JAVAARRAYOFBYTE_LOAD(currentThread, j9array, index);
						break;
					case POK_LONG:
						value.j = J9JAVAARRAYOFBYTE_LOAD(currentThread, j9array, index);
						break;
					default:
						invalidArgument = TRUE;
						break;
					}
				} else if (vm->shortReflectClass == typeOfArray) {
					switch (wCode) {
					case POK_FLOAT:
						value.f = static_cast<jfloat>(J9JAVAARRAYOFSHORT_LOAD(currentThread, j9array, index));
						break;
					case POK_DOUBLE:
						value.d = static_cast<jdouble>(J9JAVAARRAYOFSHORT_LOAD(currentThread, j9array, index));
						break;
					case POK_SHORT:
						value.s = J9JAVAARRAYOFSHORT_LOAD(currentThread, j9array, index);
						break;
					case POK_INT:
						value.i = J9JAVAARRAYOFSHORT_LOAD(currentThread, j9array, index);
						break;
					case POK_LONG:
						value.j = J9JAVAARRAYOFSHORT_LOAD(currentThread, j9array, index);
						break;
					default:
						invalidArgument = TRUE;
						break;
					}
				} else if (vm->intReflectClass == typeOfArray) {
					switch (wCode) {
					case POK_FLOAT:
						value.f = static_cast<jfloat>(J9JAVAARRAYOFINT_LOAD(currentThread, j9array, index));
						break;
					case POK_DOUBLE:
						value.d = static_cast<jdouble>(J9JAVAARRAYOFINT_LOAD(currentThread, j9array, index));
						break;
					case POK_INT:
						value.i = J9JAVAARRAYOFINT_LOAD(currentThread, j9array, index);
						break;
					case POK_LONG:
						value.j = J9JAVAARRAYOFINT_LOAD(currentThread, j9array, index);
						break;
					default: invalidArgument = TRUE; break;
					}
				} else if (vm->longReflectClass == typeOfArray) {
					switch (wCode) {
					case POK_FLOAT:
						value.f = static_cast<jfloat>(J9JAVAARRAYOFLONG_LOAD(currentThread, j9array, index));
						break;
					case POK_DOUBLE:
						value.d = static_cast<jdouble>(J9JAVAARRAYOFLONG_LOAD(currentThread, j9array, index));
						break;
					case POK_LONG:
						value.j = J9JAVAARRAYOFLONG_LOAD(currentThread, j9array, index);
						break;
					default:
						invalidArgument = TRUE;
						break;
					}
				} else {
					invalidArgument = TRUE;
				}
				if (invalidArgument) {
					vmFuncs->setCurrentException(
					        currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION, nullptr);
				}
			}
		} else {
			vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
			                             nullptr);
		}
	}
	vmFuncs->internalExitVMToJNI(currentThread);

	return value;
}

jobject JNICALL
JVM_GetProtectionDomain(jint arg0, jint arg1)
{
	assert(!"JVM_GetProtectionDomain() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetStackAccessControlContext(JNIEnv *env, jclass java_security_AccessController)
{
	return nullptr;
}

jint JNICALL
JVM_GetStackTraceDepth(JNIEnv *env, jobject throwable)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmfns = vm->internalVMFunctions;
	jint numberOfFrames = 0;
	UDATA pruneConstructors = 0;
	/* If -XX:+ShowHiddenFrames option has not been set, skip hidden method frames */
	UDATA skipHiddenFrames = J9_ARE_NO_BITS_SET(vm->runtimeFlags, J9_RUNTIME_SHOW_HIDDEN_FRAMES);

	vmfns->internalEnterVMFromJNI(currentThread);
	numberOfFrames = static_cast<jint>(vmfns->iterateStackTrace(currentThread,
																reinterpret_cast<j9object_t *>(throwable),
																nullptr,
																nullptr,
	                											pruneConstructors,
																skipHiddenFrames));
	vmfns->internalExitVMToJNI(currentThread);

	return numberOfFrames;
}

static jclass
java_lang_StackTraceElement(JNIEnv *env)
{
	static jclass cached = nullptr;
	jclass localRef = nullptr;

	if (nullptr != cached) {
		return cached;
	}

	localRef = env->FindClass("java/lang/StackTraceElement");
	assert(localRef != nullptr);

	cached = static_cast<jclass>(env->NewGlobalRef(localRef));
	if (cached == nullptr) {
		return nullptr;
	}

	env->DeleteLocalRef(localRef);
	assert(localRef != nullptr);
	return cached;
}

static jmethodID
java_lang_StackTraceElement_init(JNIEnv *env)
{
	static jmethodID cached = nullptr;
	if (nullptr != cached) {
		return cached;
	}

	cached = env->GetMethodID(java_lang_StackTraceElement(env), "<init>",
	                          "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");
	assert(cached != nullptr);
	return cached;
}

typedef struct GetStackTraceElementUserData
{
	J9ROMClass *romClass;
	J9ROMMethod *romMethod;
	J9UTF8 *fileName;
	UDATA lineNumber;
	J9ClassLoader *classLoader;
	UDATA seekFrameIndex;
	UDATA currentFrameIndex;
	bool found;
} GetStackTraceElementUserData;

/* Return TRUE to keep iterating, FALSE to halt the walk. */
static UDATA
getStackTraceElementIterator(J9VMThread *vmThread,
                             void *voidUserData,
                             UDATA bytecodeOffset,
                             J9ROMClass *romClass,
                             J9ROMMethod *romMethod,
                             J9UTF8 *fileName,
                             UDATA lineNumber,
                             J9ClassLoader *classLoader,
                             J9Class *ramClass)
{
	GetStackTraceElementUserData *userData = reinterpret_cast<GetStackTraceElementUserData *>(voidUserData);

	if (userData->seekFrameIndex == userData->currentFrameIndex) {
		/* We are done, remember the current state and return */
		userData->romClass = romClass;
		userData->romMethod = romMethod;
		userData->fileName = fileName;
		userData->lineNumber = lineNumber;
		userData->classLoader = classLoader;
		userData->found = TRUE;
		return FALSE;
	}

	userData->currentFrameIndex++;
	return TRUE;
}

jobject JNICALL
JVM_GetStackTraceElement(JNIEnv *env, jobject throwable, jint index)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmfns = vm->internalVMFunctions;
	jobject stackTraceElement = nullptr;
	UDATA pruneConstructors = 0;
	GetStackTraceElementUserData userData;
	jobject declaringClass = nullptr;
	jobject methodName = nullptr;
	jobject fileName = nullptr;
	jint lineNumber = -1;
	/* If -XX:+ShowHiddenFrames option has not been set, skip hidden method frames */
	UDATA skipHiddenFrames = J9_ARE_NO_BITS_SET(vm->runtimeFlags, J9_RUNTIME_SHOW_HIDDEN_FRAMES);

	memset(&userData, 0, sizeof(userData));
	userData.seekFrameIndex = index;

	vmfns->internalEnterVMFromJNI(currentThread);
	vmfns->iterateStackTrace(currentThread, reinterpret_cast<j9object_t *>(throwable), getStackTraceElementIterator, &userData,
	                         pruneConstructors, skipHiddenFrames);
	vmfns->internalExitVMToJNI(currentThread);

	/* Bail if we couldn't find the frame */
	if (TRUE != userData.found) {
		return nullptr;
	}

	/* Convert to Java format */
	declaringClass = utf8_to_java_lang_String(env, J9ROMCLASS_CLASSNAME(userData.romClass));
	methodName = utf8_to_java_lang_String(env, J9ROMMETHOD_NAME(userData.romMethod));
	fileName = utf8_to_java_lang_String(env, userData.fileName);
	lineNumber = static_cast<jint>(userData.lineNumber);

	stackTraceElement = env->NewObject(java_lang_StackTraceElement(env), java_lang_StackTraceElement_init(env),
	                                   declaringClass, methodName, fileName, lineNumber);

	assert(nullptr != stackTraceElement);
	return stackTraceElement;
}

jobject JNICALL
JVM_HoldsLock(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_HoldsLock() stubbed!");
	return nullptr;
}

/**
 * Get hashCode of the object. This function may lock, gc or throw exception.
 * @return hashCode, if obj is not nullptr.
 * @return 0, if obj is nullptr.
 */
jint JNICALL
JVM_IHashCode(JNIEnv *env, jobject obj)
{
	jint result = 0;

	if (obj != nullptr) {
		J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
		J9JavaVM *vm = currentThread->javaVM;
		J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;

		vmFuncs->internalEnterVMFromJNI(currentThread);
		result = vm->memoryManagerFunctions->j9gc_objaccess_getObjectHashCode(vm, J9_JNI_UNWRAP_REFERENCE(obj));
		vmFuncs->internalExitVMToJNI(currentThread);
	}

	return result;
}

jobject JNICALL
JVM_InitProperties(JNIEnv *env, jobject properties)
{
	/* This JVM method is invoked by JCL native Java_java_lang_System_initProperties
	 * only for initialization of platform encoding.
	 * This is only required by Java 11 raw builds.
	 * This method is not invoked by other Java levels.
	 */
#if JAVA_SPEC_VERSION < 11
	assert(!"JVM_InitProperties should not be called!");
#endif /* JAVA_SPEC_VERSION < 11 */
	return properties;
}

/**
 * Returns a canonical representation for the string object. If the string is already in the pool, just return
 * the string. If not, add the string to the pool and return the string.
 * This function may lock, gc or throw exception.
 * @param str The string object.
 * @return nullptr if str is nullptr
 * @return The interned string
 */
jstring JNICALL
JVM_InternString(JNIEnv *env, jstring str)
{
	if (str != nullptr) {
		J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
		J9JavaVM *javaVM = currentThread->javaVM;
		J9InternalVMFunctions *vmfns = javaVM->internalVMFunctions;
		j9object_t stringObject = nullptr;

		vmfns->internalEnterVMFromJNI(currentThread);
		stringObject = J9_JNI_UNWRAP_REFERENCE(str);
		stringObject = javaVM->memoryManagerFunctions->j9gc_internString(currentThread, stringObject);
		str = static_cast<jstring>(vmfns->j9jni_createLocalRef(env, stringObject));
		vmfns->internalExitVMToJNI(currentThread);
	}

	return str;
}

jobject JNICALL
JVM_Interrupt(jint arg0, jint arg1)
{
	assert(!"JVM_Interrupt() stubbed!");
	return nullptr;
}

jboolean JNICALL
JVM_IsArrayClass(JNIEnv *env, jclass clazz)
{
	J9Class *ramClass = java_lang_Class_vmRef(env, clazz);
	if (J9ROMCLASS_IS_ARRAY(ramClass->romClass)) {
		return JNI_TRUE;
	} else {
		return JNI_FALSE;
	}
}

jboolean JNICALL
JVM_IsInterface(JNIEnv *env, jclass clazz)
{
	J9Class *ramClass = java_lang_Class_vmRef(env, clazz);
	if (J9ROMCLASS_IS_INTERFACE(ramClass->romClass)) {
		return JNI_TRUE;
	} else {
		return JNI_FALSE;
	}
}

jboolean JNICALL
JVM_IsInterrupted(JNIEnv *env, jobject thread, jboolean unknown)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9VMThread *targetThread = nullptr;
	J9JavaVM *vm = currentThread->javaVM;
	J9ThreadEnv *threadEnv = getJ9ThreadEnv(env);
	UDATA rcClear = 0;

	vm->internalVMFunctions->internalEnterVMFromJNI(currentThread);
	targetThread = J9VMJAVALANGTHREAD_THREADREF(currentThread, J9_JNI_UNWRAP_REFERENCE(thread));
	vm->internalVMFunctions->internalExitVMToJNI(currentThread);

	assert(targetThread == currentThread);

	if (nullptr != vm->sidecarClearInterruptFunction) {
		vm->sidecarClearInterruptFunction(currentThread);
	}

	rcClear = threadEnv->clear_interrupted();
	if (0 != rcClear) {
		return JNI_TRUE;
	} else {
		return JNI_FALSE;
	}
}

jboolean JNICALL
JVM_IsPrimitiveClass(JNIEnv *env, jclass clazz)
{
	J9Class *ramClass = java_lang_Class_vmRef(env, clazz);
	if (J9ROMCLASS_IS_PRIMITIVE_TYPE(ramClass->romClass)) {
		return JNI_TRUE;
	} else {
		return JNI_FALSE;
	}
}

/**
 * Check whether the JNI version is supported.
 * This function may not lock, GC or throw an exception.
 * @param version
 * @return true if version is supported; false if not
 * @careful
 */
jboolean JNICALL
JVM_IsSupportedJNIVersion(jint version)
{
	switch (version) {
	case JNI_VERSION_1_1:
	case JNI_VERSION_1_2:
	case JNI_VERSION_1_4:
	case JNI_VERSION_1_6:
	case JNI_VERSION_1_8:
#if JAVA_SPEC_VERSION >= 9
	case JNI_VERSION_9:
#endif /* JAVA_SPEC_VERSION >= 9 */
#if JAVA_SPEC_VERSION >= 10
	case JNI_VERSION_10:
#endif /* JAVA_SPEC_VERSION >= 10 */
#if JAVA_SPEC_VERSION >= 19
	case JNI_VERSION_19:
#endif /* JAVA_SPEC_VERSION >= 19 */
#if JAVA_SPEC_VERSION >= 20
	case JNI_VERSION_20:
#endif /* JAVA_SPEC_VERSION >= 20 */
#if JAVA_SPEC_VERSION >= 21
	case JNI_VERSION_21:
#endif /* JAVA_SPEC_VERSION >= 21 */
		return JNI_TRUE;

	default:
		return JNI_FALSE;
	}
}

#if JAVA_SPEC_VERSION < 17
jboolean JNICALL
JVM_IsThreadAlive(JNIEnv *env, jobject targetThread)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9VMThread *vmThread;

	vm->internalVMFunctions->internalEnterVMFromJNI(currentThread);
	vmThread = J9VMJAVALANGTHREAD_THREADREF(currentThread, J9_JNI_UNWRAP_REFERENCE(targetThread));
	vm->internalVMFunctions->internalExitVMToJNI(currentThread);

	/* Assume that a non-null threadRef indicates the thread is alive */
	return (nullptr == vmThread) ? JNI_FALSE : JNI_TRUE;
}
#endif /* JAVA_SPEC_VERSION < 17 */

jobject JNICALL
JVM_NewArray(JNIEnv *env, jclass componentType, jint dimension)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9MemoryManagerFunctions *mmfns = vm->memoryManagerFunctions;
	J9Class *ramClass = java_lang_Class_vmRef(env, componentType);
	J9ROMClass *romClass = ramClass->romClass;
	j9object_t newArray = nullptr;
	jobject arrayRef = nullptr;

	vm->internalVMFunctions->internalEnterVMFromJNI(currentThread);
	if (ramClass->arrayClass == nullptr) {
		vm->internalVMFunctions->setCurrentException(currentThread,
		                                             J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION, nullptr);
		return nullptr;
	}

	newArray = vm->memoryManagerFunctions->J9AllocateIndexableObject(currentThread, ramClass->arrayClass, dimension,
	                                                                 J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);

	if (nullptr == newArray) {
		vm->internalVMFunctions->setHeapOutOfMemoryError(currentThread);
		return nullptr;
	}

	arrayRef = vm->internalVMFunctions->j9jni_createLocalRef(env, newArray);
	vm->internalVMFunctions->internalExitVMToJNI(currentThread);
	return arrayRef;
}

static J9Class *
fetchArrayClass(struct J9VMThread *vmThread, J9Class *elementTypeClass)
{
	/* Quick check before grabbing the mutex */
	J9Class *resultClass = elementTypeClass->arrayClass;
	if (nullptr == resultClass) {
		/* Allocate an array class */
		J9ROMArrayClass *arrayOfObjectsROMClass =
		        reinterpret_cast<J9ROMArrayClass *>(J9ROMIMAGEHEADER_FIRSTCLASS(vmThread->javaVM->arrayROMClasses));

		resultClass = vmThread->javaVM->internalVMFunctions->internalCreateArrayClass(
		        vmThread, arrayOfObjectsROMClass, elementTypeClass);
	}
	return resultClass;
}

/**
 * Allocate a multi-dimension array with class specified
 * This function may lock, gc or throw exception.
 * @param eltClass The class of the element
 * @param dim The dimension
 * @return The newly allocated array
 */
jobject JNICALL
JVM_NewMultiArray(JNIEnv *env, jclass eltClass, jintArray dim)
{
	/* Maximum array dimensions, according to the spec for the array bytecodes, is 255 */
	constexpr UDATA MAX_DIMENSIONS = 255;
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9InternalVMFunctions *vmFuncs = currentThread->javaVM->internalVMFunctions;
	jobject result = nullptr;

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if (nullptr == dim) {
		vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, nullptr);
	} else {
		j9object_t dimensionsArrayObject = J9_JNI_UNWRAP_REFERENCE(dim);
		UDATA dimensions = J9INDEXABLEOBJECT_SIZE(currentThread, dimensionsArrayObject);

		dimensionsArrayObject = nullptr; /* must be refetched after GC points below */
		if (dimensions > MAX_DIMENSIONS) {
			/* the spec says to throw this exception if the number of dimensions in greater than the count we support (and a nullptr message appears to be the behaviour of the reference implementation) */
			vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
			                             nullptr);
		} else {
			j9object_t componentTypeClassObject = J9_JNI_UNWRAP_REFERENCE(eltClass);

			if (nullptr != componentTypeClassObject) {
				J9Class *componentTypeClass =
				        J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, componentTypeClassObject);

				/* create an array class with the desired arity */
				UDATA count = dimensions;
				J9Class *componentArrayClass = componentTypeClass;
				bool exceptionIsPending = FALSE;

				while ((count > 0) && (!exceptionIsPending)) {
					componentArrayClass = fetchArrayClass(currentThread, componentArrayClass);
					exceptionIsPending = (nullptr != currentThread->currentException);
					count -= 1;
				}

				if (!exceptionIsPending) {
					/* make a copy of the dimensions array in non-object memory */
					I_32 onStackDimensions[MAX_DIMENSIONS];
					j9object_t directObject = nullptr;
					UDATA i = 0;

					memset(onStackDimensions, 0, sizeof(onStackDimensions));
					dimensionsArrayObject = J9_JNI_UNWRAP_REFERENCE(dim);
					for (i = 0; i < dimensions; i++) {
						onStackDimensions[i] =
						        J9JAVAARRAYOFINT_LOAD(currentThread, dimensionsArrayObject, i);
					}

					directObject = vmFuncs->helperMultiANewArray(
					        currentThread, reinterpret_cast<J9ArrayClass *>(componentArrayClass), static_cast<UDATA>(dimensions),
					        onStackDimensions, J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
					if (nullptr != directObject) {
						result = vmFuncs->j9jni_createLocalRef(env, directObject);
					}
				}
			}
		}
	}

	vmFuncs->internalExitVMToJNI(currentThread);
	return result;
}

jobject JNICALL
JVM_ResolveClass(jint arg0, jint arg1)
{
	assert(!"JVM_ResolveClass() stubbed!");
	return nullptr;
}

/**
 * Set the val to the array at the index.
 * This function may lock, gc or throw exception.
 * @param array The array
 * @param index The index
 * @param value The set value
 */
void JNICALL
JVM_SetArrayElement(JNIEnv *env, jobject array, jint index, jobject value)
{
	J9VMThread *currentThread = nullptr;
	J9JavaVM *vm = nullptr;
	J9InternalVMFunctions *vmFuncs = nullptr;

	Assert_SC_notNull(env);

	currentThread = reinterpret_cast<J9VMThread *>(env);
	vm = currentThread->javaVM;
	vmFuncs = vm->internalVMFunctions;

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if (nullptr == array) {
		vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, nullptr);
	} else {
		j9object_t j9array = J9_JNI_UNWRAP_REFERENCE(array);
		J9ArrayClass *arrayClass = reinterpret_cast<J9ArrayClass *>(J9OBJECT_CLAZZ(currentThread, j9array));
		J9Class *typeOfArray = arrayClass->componentType;

		if (J9CLASS_IS_ARRAY(arrayClass)) {
			if ((index < 0) || (static_cast<jint>(J9INDEXABLEOBJECT_SIZE(currentThread, j9array)) <= index)) {
				vmFuncs->setCurrentException(
				        currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYINDEXOUTOFBOUNDSEXCEPTION, nullptr);
			} else {
				if (J9ROMCLASS_IS_PRIMITIVE_TYPE(typeOfArray->romClass)) {
					if (nullptr == value) {
						vmFuncs->setCurrentException(
						        currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION,
						        nullptr);
					} else {
						J9Class *booleanWrapperClass = J9VMJAVALANGBOOLEAN_OR_NULL(vm);
						J9Class *byteWrapperClass = J9VMJAVALANGBYTE_OR_NULL(vm);
						J9Class *shortWrapperClass = J9VMJAVALANGSHORT_OR_NULL(vm);
						J9Class *charWrapperClass = J9VMJAVALANGCHARACTER_OR_NULL(vm);
						J9Class *intWrapperClass = J9VMJAVALANGINTEGER_OR_NULL(vm);
						J9Class *floatWrapperClass = J9VMJAVALANGFLOAT_OR_NULL(vm);
						J9Class *doubleWrapperClass = J9VMJAVALANGDOUBLE_OR_NULL(vm);
						J9Class *longWrapperClass = J9VMJAVALANGLONG_OR_NULL(vm);

						bool invalidArgument = FALSE;
						j9object_t j9value = J9_JNI_UNWRAP_REFERENCE(value);
						J9Class *valueClass = J9OBJECT_CLAZZ(currentThread, j9value);

						if (vm->longReflectClass == typeOfArray) {
							jlong val = 0;
							if (longWrapperClass == valueClass) {
								val = J9VMJAVALANGLONG_VALUE(currentThread, j9value);
							} else if (intWrapperClass == valueClass) {
								val = static_cast<jlong>(J9VMJAVALANGINTEGER_VALUE(currentThread, j9value));
							} else if (shortWrapperClass == valueClass) {
								val = static_cast<jlong>(J9VMJAVALANGSHORT_VALUE(currentThread, j9value));
							} else if (charWrapperClass == valueClass) {
								val = static_cast<jlong>(J9VMJAVALANGCHARACTER_VALUE(currentThread, j9value));
							} else if (byteWrapperClass == valueClass) {
								val = static_cast<jlong>(J9VMJAVALANGBYTE_VALUE(currentThread, j9value));
							} else {
								invalidArgument = TRUE;
							}
							if (!invalidArgument) {
								J9JAVAARRAYOFLONG_STORE(currentThread, j9array, index, val);
							}
						} else if (vm->booleanReflectClass == typeOfArray) {
							if (booleanWrapperClass == valueClass) {
								J9JAVAARRAYOFBOOLEAN_STORE(currentThread,
														   j9array,
														   index,
														   J9VMJAVALANGBOOLEAN_VALUE(currentThread, j9value));
							} else {
								invalidArgument = TRUE;
							}
						} else if (vm->byteReflectClass == typeOfArray) {
							if (byteWrapperClass == valueClass) {
								J9JAVAARRAYOFBYTE_STORE(currentThread,
														j9array,
														index,
														J9VMJAVALANGBYTE_VALUE(currentThread, j9value));
							} else {
								invalidArgument = TRUE;
							}
						} else if (vm->charReflectClass == typeOfArray) {
							if (charWrapperClass == valueClass) {
								J9JAVAARRAYOFCHAR_STORE(currentThread,
														j9array,
														index,
														J9VMJAVALANGCHARACTER_VALUE(currentThread, j9value));
							} else {
								invalidArgument = TRUE;
							}
						} else if (vm->shortReflectClass == typeOfArray) {
							jshort val = 0;
							if (shortWrapperClass == valueClass) {
								val = J9VMJAVALANGSHORT_VALUE(currentThread, j9value);
							} else if (byteWrapperClass == valueClass) {
								val = static_cast<jshort>(J9VMJAVALANGBYTE_VALUE(currentThread, j9value));
							} else {
								invalidArgument = TRUE;
							}
							if (!invalidArgument) {
								J9JAVAARRAYOFSHORT_STORE(currentThread, j9array, index, val);
							}
						} else if (vm->intReflectClass == typeOfArray) {
							jint val = 0;
							if (intWrapperClass == valueClass) {
								val = J9VMJAVALANGINTEGER_VALUE(currentThread, j9value);
							} else if (shortWrapperClass == valueClass) {
								val = static_cast<jint>(J9VMJAVALANGSHORT_VALUE(currentThread, j9value));
							} else if (charWrapperClass == valueClass) {
								val = static_cast<jint>(J9VMJAVALANGCHARACTER_VALUE(currentThread, j9value));
							} else if (byteWrapperClass == valueClass) {
								val = static_cast<jint>(J9VMJAVALANGBYTE_VALUE(currentThread, j9value));
							} else {
								invalidArgument = TRUE;
							}
							if (!invalidArgument) {
								J9JAVAARRAYOFINT_STORE(currentThread, j9array, index, val);
							}
						} else if (vm->floatReflectClass == typeOfArray) {
							jfloat val = 0;
							if (floatWrapperClass == valueClass) {
								*reinterpret_cast<U_32 *>(&val) = J9VMJAVALANGFLOAT_VALUE(currentThread, j9value);
							} else if (longWrapperClass == valueClass) {
								val = static_cast<jfloat>(J9VMJAVALANGLONG_VALUE(currentThread, j9value));
							} else if (intWrapperClass == valueClass) {
								val = static_cast<jfloat>(static_cast<I_32>J9VMJAVALANGINTEGER_VALUE(currentThread, j9value));
							} else if (shortWrapperClass == valueClass) {
								val = static_cast<jfloat>(static_cast<I_32>(J9VMJAVALANGSHORT_VALUE(currentThread, j9value)));
							} else if (charWrapperClass == valueClass) {
								val = static_cast<jfloat>(J9VMJAVALANGCHARACTER_VALUE(currentThread, j9value));
							} else if (byteWrapperClass == valueClass) {
								val = static_cast<jfloat>(static_cast<I_32>(J9VMJAVALANGBYTE_VALUE(currentThread, j9value)));
							} else {
								invalidArgument = TRUE;
							}
							if (!invalidArgument) {
								J9JAVAARRAYOFFLOAT_STORE(currentThread, j9array, index, *reinterpret_cast<U_32 *>(&val));
							}
						} else if (vm->doubleReflectClass == typeOfArray) {
							jdouble val = 0;
							if (doubleWrapperClass == valueClass) {
								*reinterpret_cast<U_64 *>(&val) = J9VMJAVALANGDOUBLE_VALUE(currentThread, j9value);
							} else if (floatWrapperClass == valueClass) {
								jfloat floatNumber;
								*reinterpret_cast<U_64 *>(&floatNumber) = J9VMJAVALANGFLOAT_VALUE(currentThread, j9value);
								val = static_cast<jdouble>(floatNumber);
							} else if (longWrapperClass == valueClass) {
								val = static_cast<jdouble>(J9VMJAVALANGLONG_VALUE(currentThread, j9value));
							} else if (intWrapperClass == valueClass) {
								val =  static_cast<jdouble>(static_cast<I_32>(J9VMJAVALANGINTEGER_VALUE(currentThread, j9value)));
							} else if (shortWrapperClass == valueClass) {
								val = static_cast<jdouble>(static_cast<I_32>(J9VMJAVALANGSHORT_VALUE(currentThread, j9value)));
							} else if (charWrapperClass == valueClass) {
								val = static_cast<jdouble>(J9VMJAVALANGCHARACTER_VALUE(currentThread, j9value));
							} else if (byteWrapperClass == valueClass) {
								val = static_cast<jdouble>(static_cast<I_32>(J9VMJAVALANGBYTE_VALUE(currentThread, j9value)));
							} else {
								invalidArgument = TRUE;
							}
							if (!invalidArgument) {
								J9JAVAARRAYOFDOUBLE_STORE(currentThread, j9array, index, *reinterpret_cast<U_64 *>(&val));
							}
						} else {
							invalidArgument = TRUE;
						}
						if (invalidArgument) {
							vmFuncs->setCurrentException(
							        currentThread,
							        J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
							        nullptr);
						}
					}
				} else {
					if (nullptr == value) {
						J9JAVAARRAYOFOBJECT_STORE(currentThread, j9array, index, value);
					} else {
						j9object_t j9value = J9_JNI_UNWRAP_REFERENCE(value);
						J9Class *valueClass = J9OBJECT_CLAZZ(currentThread, j9value);

						if (isSameOrSuperClassOf(reinterpret_cast<J9Class *>(arrayClass->componentType), valueClass)) {
							J9JAVAARRAYOFOBJECT_STORE(currentThread, j9array, index, j9value);
						} else {
							vmFuncs->setCurrentException(
							        currentThread,
							        J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
							        nullptr);
						}
					}
				}
			}
		} else {
			vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
			                             nullptr);
		}
	}
	vmFuncs->internalExitVMToJNI(currentThread);

	return;
}

jobject JNICALL
JVM_SetClassSigners(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_SetClassSigners() stubbed!");
	return nullptr;
}

/**
 * Set the value to the primitive array at the index.
 * This function may lock, gc or throw exception.
 * @param array The array
 * @param index The index
 * @param value The set value
 * @param vCode The primitive symbol type code
 */
void JNICALL
JVM_SetPrimitiveArrayElement(JNIEnv *env, jobject array, jint index, jvalue value, unsigned char vCode)
{
	J9VMThread *currentThread = nullptr;
	J9JavaVM *vm = nullptr;
	J9InternalVMFunctions *vmFuncs = nullptr;

	Assert_SC_notNull(env);

	currentThread = reinterpret_cast<J9VMThread *>(env);
	vm = currentThread->javaVM;
	vmFuncs = vm->internalVMFunctions;

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if (nullptr == array) {
		vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, nullptr);
	} else {
		j9object_t j9array = J9_JNI_UNWRAP_REFERENCE(array);
		J9ArrayClass *arrayClass = reinterpret_cast<J9ArrayClass *>(J9OBJECT_CLAZZ(currentThread, j9array));
		J9Class *typeOfArray = arrayClass->componentType;

		if (J9CLASS_IS_ARRAY(arrayClass) && J9ROMCLASS_IS_PRIMITIVE_TYPE(typeOfArray->romClass)) {
			if ((index < 0) || ((static_cast<jint>(J9INDEXABLEOBJECT_SIZE(currentThread, j9array)) <= index))) {
				vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGARRAYINDEXOUTOFBOUNDSEXCEPTION, nullptr);
			} else {
				bool invalidArgument = FALSE;

				if (vm->intReflectClass == typeOfArray) {
					switch (vCode) {
					case POK_CHAR:
						J9JAVAARRAYOFINT_STORE(currentThread, j9array, index, value.c);
						break;
					case POK_BYTE:
						J9JAVAARRAYOFINT_STORE(currentThread, j9array, index, value.b);
						break;
					case POK_SHORT:
						J9JAVAARRAYOFINT_STORE(currentThread, j9array, index, value.s);
						break;
					case POK_INT:
						J9JAVAARRAYOFINT_STORE(currentThread, j9array, index, value.i);
						break;
					default:
						invalidArgument = TRUE;
						break;
					}
				} else if (vm->longReflectClass == typeOfArray) {
					switch (vCode) {
					case POK_CHAR:
						J9JAVAARRAYOFLONG_STORE(currentThread, j9array, index, value.c);
						break;
					case POK_BYTE:
						J9JAVAARRAYOFLONG_STORE(currentThread, j9array, index, value.b);
						break;
					case POK_SHORT:
						J9JAVAARRAYOFLONG_STORE(currentThread, j9array, index, value.s);
						break;
					case POK_INT:
						J9JAVAARRAYOFLONG_STORE(currentThread, j9array, index, value.i);
						break;
					case POK_LONG:
						J9JAVAARRAYOFLONG_STORE(currentThread, j9array, index, value.j);
						break;
					default:
						invalidArgument = TRUE;
						break;
					}
				} else if (vm->byteReflectClass == typeOfArray) {
					if (POK_BYTE == vCode) {
						J9JAVAARRAYOFBYTE_STORE(currentThread, j9array, index, value.b);
					} else {
						invalidArgument = TRUE;
					}
				} else if (vm->doubleReflectClass == typeOfArray) {
					jdouble val = 0;
					switch (vCode) {
					case POK_CHAR:
						val = static_cast<jdouble>(value.c);
						break;
					case POK_FLOAT:
						val = static_cast<jdouble>(value.f);
						break;
					case POK_DOUBLE:
						val = value.d;
						break;
					case POK_BYTE:
						val = static_cast<jdouble>(value.b);
						break;
					case POK_SHORT:
						val = static_cast<jdouble>(value.s);
						break;
					case POK_INT:
						val = static_cast<jdouble>(value.i);
						break;
					case POK_LONG:
						val = static_cast<jdouble>(value.j);
						break;
					default:
						invalidArgument = TRUE;
						break;
					}
					if (!invalidArgument) {
						J9JAVAARRAYOFDOUBLE_STORE(currentThread, j9array, index, *reinterpret_cast<U_64 *>(&val));
					}
				} else if (vm->floatReflectClass == typeOfArray) {
					jfloat val = 0;
					switch (vCode) {
					case POK_CHAR:
						val = static_cast<jfloat>(value.c);
						break;
					case POK_FLOAT:
						val = value.f;
						break;
					case POK_BYTE:
						val = static_cast<jfloat>(value.b);
						break;
					case POK_SHORT:
						val = static_cast<jfloat>(value.s);
						break;
					case POK_INT:
						val = static_cast<jfloat>(value.i);
						break;
					case POK_LONG:
						val = static_cast<jfloat>(value.j);
						break;
					default:
						invalidArgument = TRUE;
						break;
					}
					if (!invalidArgument) {
						J9JAVAARRAYOFFLOAT_STORE(currentThread, j9array, index, *reinterpret_cast<U_32 *>(&val));
					}
				} else if (vm->shortReflectClass == typeOfArray) {
					switch (vCode) {
					case POK_BYTE:
						J9JAVAARRAYOFSHORT_STORE(currentThread, j9array, index, value.b);
						break;
					case POK_SHORT:
						J9JAVAARRAYOFSHORT_STORE(currentThread, j9array, index, value.s);
						break;
					default:
						invalidArgument = TRUE;
						break;
					}
				} else if (vm->charReflectClass == typeOfArray) {
					if (POK_CHAR == vCode) {
						J9JAVAARRAYOFCHAR_STORE(currentThread, j9array, index, value.c);
					} else {
						invalidArgument = TRUE;
					}
				} else if ((vm->booleanReflectClass == typeOfArray) && (4 == vCode)) {
					J9JAVAARRAYOFBOOLEAN_STORE(currentThread, j9array, index, value.z);
				} else {
					invalidArgument = TRUE;
				}
				if (invalidArgument) {
					vmFuncs->setCurrentException(
					        currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION, nullptr);
				}
			}
		} else {
			vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
			                             nullptr);
		}
	}
	vmFuncs->internalExitVMToJNI(currentThread);

	return;
}

jobject JNICALL
JVM_SetProtectionDomain(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_SetProtectionDomain() stubbed!");
	return nullptr;
}

void JNICALL
JVM_SetThreadPriority(JNIEnv *env, jobject thread, jint priority)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	const UDATA *prioMap = currentThread->javaVM->java2J9ThreadPriorityMap;
	J9VMThread *vmThread = nullptr;

	if (currentThread->javaVM->runtimeFlags & J9_RUNTIME_NO_PRIORITIES) {
		return;
	}

	assert(prioMap != nullptr);
	assert(priority >= 0);
	assert(priority < sizeof(currentThread->javaVM->java2J9ThreadPriorityMap)
	               / sizeof(currentThread->javaVM->java2J9ThreadPriorityMap[0]));

	vm = currentThread->javaVM;
	vm->internalVMFunctions->internalEnterVMFromJNI(currentThread);
	vmThread = J9VMJAVALANGTHREAD_THREADREF(currentThread, J9_JNI_UNWRAP_REFERENCE(thread));
	vm->internalVMFunctions->internalExitVMToJNI(currentThread);

	if ((nullptr != vmThread) && (nullptr != vmThread->osThread)) {
		J9ThreadEnv *threadEnv = getJ9ThreadEnv(env);
		threadEnv->set_priority(vmThread->osThread, prioMap[priority]);
	}
}

void JNICALL
JVM_StartThread(JNIEnv *env, jobject newThread)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *javaVM = currentThread->javaVM;
	UDATA priority = J9THREAD_PRIORITY_NORMAL;
	UDATA isDaemon = FALSE;
	UDATA privateFlags = 0;
	j9object_t newThreadObject = nullptr;
	UDATA result = 0;

	javaVM->internalVMFunctions->internalEnterVMFromJNI(currentThread);

	newThreadObject = J9_JNI_UNWRAP_REFERENCE(newThread);
#if JAVA_SPEC_VERSION >= 19
	j9object_t threadHolder = J9VMJAVALANGTHREAD_HOLDER(currentThread, newThreadObject);
#endif /* JAVA_SPEC_VERSION >= 19 */

	if (J9_ARE_NO_BITS_SET(javaVM->runtimeFlags, J9RuntimeFlagNoPriorities)) {
#if JAVA_SPEC_VERSION >= 19
		if (nullptr != threadHolder) {
			priority = J9VMJAVALANGTHREADFIELDHOLDER_PRIORITY(currentThread, threadHolder);
		}
#else /* JAVA_SPEC_VERSION >= 19 */
		priority = J9VMJAVALANGTHREAD_PRIORITY(currentThread, newThreadObject);
#endif /* JAVA_SPEC_VERSION >= 19 */
	}

#if JAVA_SPEC_VERSION >= 19
	if (nullptr != threadHolder) {
		isDaemon = J9VMJAVALANGTHREADFIELDHOLDER_DAEMON(currentThread, threadHolder);
	}
#else /* JAVA_SPEC_VERSION >= 19 */
	isDaemon = J9VMJAVALANGTHREAD_ISDAEMON(currentThread, newThreadObject);
#endif /* JAVA_SPEC_VERSION >= 19 */
	if (isDaemon) {
		privateFlags = J9_PRIVATE_FLAGS_DAEMON_THREAD;
	}

	result = javaVM->internalVMFunctions->startJavaThread(
	        currentThread, newThreadObject,
	        J9_PRIVATE_FLAGS_DAEMON_THREAD | J9_PRIVATE_FLAGS_NO_EXCEPTION_IN_START_JAVA_THREAD,
	        javaVM->defaultOSStackSize, priority,
	        reinterpret_cast<omrthread_entrypoint_t>(javaVM->internalVMFunctions->javaThreadProc), javaVM, nullptr);

	javaVM->internalVMFunctions->internalExitVMToJNI(currentThread);

	if (result != J9_THREAD_START_NO_ERROR) {
		assert(!"JVM_StartThread() failed!");
	}

	return;
}

#if JAVA_SPEC_VERSION < 20
jobject JNICALL
JVM_ResumeThread(jint arg0, jint arg1)
{
	assert(!"JVM_ResumeThread() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_StopThread(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_StopThread() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_SuspendThread(jint arg0, jint arg1)
{
	assert(!"JVM_SuspendThread() stubbed!");
	return nullptr;
}
#endif /* JAVA_SPEC_VERSION < 20 */

jobject JNICALL
JVM_Yield(jint arg0, jint arg1)
{
	assert(!"JVM_Yield() stubbed!");
	return nullptr;
}

/**
 * CMVC 150207 : Used by libnet.so on linux x86.
 */
jint JNICALL
JVM_SetSockOpt(jint fd, int level, int optname, const char *optval, int optlen)
{
	jint retVal = 0;

#if defined(WIN32)
	retVal = setsockopt(fd, level, optname, optval, optlen);
#elif defined(J9ZTPF)
	retVal = setsockopt(fd, level, optname, const_cast<char *>(optval), static_cast<socklen_t>(optlen));
#else
	retVal = setsockopt(fd, level, optname, optval, static_cast<socklen_t>(optlen));
#endif

	return retVal;
}

jint JNICALL
JVM_GetSockOpt(jint fd, int level, int optname, char *optval, int *optlen)
{
	jint retVal = 0;

#if defined(WIN32)
	retVal = getsockopt(fd, level, optname, optval, optlen);
#elif defined(J9ZTPF)
	retVal = getsockopt(fd, level, optname, const_cast<char *>(optval), reinterpret_cast<socklen_t *>optlen);
#else
	retVal = getsockopt(fd, level, optname, optval, reinterpret_cast<socklen_t *>(optlen));
#endif

	return retVal;
}

/**
 * CMVC 150207 : Used by libnet.so on linux x86.
 */
jint JNICALL
JVM_SocketShutdown(jint fd, jint howto)
{
	jint retVal = 0;

#if defined(J9UNIX)
	retVal = shutdown(fd, howto);
#elif defined(WIN32) /* defined(J9UNIX) */
	retVal = closesocket(fd);
#else /* defined(J9UNIX) */
	assert(!"JVM_SocketShutdown() stubbed!");
#endif /* defined(J9UNIX) */

	return retVal;
}

/**
 * CMVC 150207 : Used by libnet.so on linux x86.
 */
jint JNICALL
JVM_GetSockName(jint fd, struct sockaddr *him, int *len)
{
	jint retVal = 0;

#if defined(WIN32)
	retVal = getsockname(fd, him, len);
#else
	retVal = getsockname(fd, him, reinterpret_cast<socklen_t *>(len));
#endif

	return retVal;
}

/**
 * CMVC 150207 : Used by libnet.so on linux x86.
 */
int JNICALL
JVM_GetHostName(char *name, int namelen)
{
	jint retVal = gethostname(name, namelen);

	return retVal;
}

/*
 * com.sun.tools.attach.VirtualMachine support
 *
 * Initialize the agent properties with the properties maintained in the VM.
 * The following properties are set by the reference implementation:
 * 	sun.java.command = name of the main class
 *  sun.jvm.flags = vm arguments passed to the launcher
 *  sun.jvm.args =
 */
/*
 * Notes:
 * 	Redirector has an implementation of JVM_InitAgentProperties.
 * 	This method is still kept within the actual jvm dll in case that a launcher uses this jvm dll directly without going through the redirector.
 * 	If this method need to be modified, the changes have to be synchronized for both versions.
 */
jobject JNICALL
JVM_InitAgentProperties(JNIEnv *env, jobject agent_props)
{
	/* CMVC 150259 : Assert in JDWP Agent
	 *   Simply returning the non-null properties instance is
	 *   sufficient to make the agent happy. */
	return agent_props;
}

/**
 * Extend boot classpath
 *
 * @param env
 * @param pathSegment		path to add to the bootclasspath
 * @return void
 *
 * Append specified path segment to the boot classpath
 */

void JNICALL
JVM_ExtendBootClassPath(JNIEnv *env, const char *pathSegment)
{
	ENSURE_VMI();

	g_VMI->JVM_ExtendBootClassPath(env, pathSegment);
}

/**
  * Throw java.lang.OutOfMemoryError
  */
void
throwNativeOOMError(JNIEnv *env, U_32 moduleName, U_32 messageNumber)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;
	vmFuncs->internalEnterVMFromJNI(currentThread);
	vmFuncs->setNativeOutOfMemoryError(currentThread, moduleName, messageNumber);
	vmFuncs->internalExitVMToJNI(currentThread);
}

/**
  * Throw java.lang.NullPointerException with the message provided
  */
void
throwNewNullPointerException(JNIEnv *env, char *message)
{
	jclass exceptionClass = env->FindClass("java/lang/NullPointerException");
	if (nullptr == exceptionClass) {
		/* Just return if we can't load the exception class. */
		return;
	}
	env->ThrowNew(exceptionClass, message);
}

/**
  * Throw java.lang.IndexOutOfBoundsException
  */
void
throwNewIndexOutOfBoundsException(JNIEnv *env, char *message)
{
	jclass exceptionClass = env->FindClass("java/lang/IndexOutOfBoundsException");
	if (nullptr == exceptionClass) {
		/* Just return if we can't load the exception class. */
		return;
	}
	env->ThrowNew(exceptionClass, message);
}

/**
  * Throw java.lang.InternalError
  */
void
throwNewInternalError(JNIEnv *env, char *message)
{
	jclass exceptionClass = env->FindClass("java/lang/InternalError");
	if (nullptr == exceptionClass) {
		/* Just return if we can't load the exception class. */
		return;
	}
	env->ThrowNew(exceptionClass, message);
}

/* Callers of this function must have already ensured that classLoaderObject has been initialized */

jclass
jvmDefineClassHelper(JNIEnv *env,
                     jobject classLoaderObject,
                     jstring className,
                     jbyte *classBytes,
                     jint offset,
                     jint length,
                     jobject protectionDomain,
                     UDATA options)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;
	J9TranslationBufferSet *dynFuncs = nullptr;
	J9ClassLoader *classLoader = nullptr;
	UDATA retried = FALSE;
	UDATA utf8Length = 0;
	char utf8NameStackBuffer[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
	U_8 *utf8Name = nullptr;
	J9Class *clazz = nullptr;
	jclass result = nullptr;
	J9ThreadEnv *threadEnv = getJ9ThreadEnv(env);
	J9ROMClass *loadedClass = nullptr;
	U_8 *tempClassBytes = nullptr;
	I_32 tempLength = 0;
	J9TranslationLocalBuffer localBuffer = {J9_CP_INDEX_NONE, LOAD_LOCATION_UNKNOWN, nullptr};
	PORT_ACCESS_FROM_JAVAVM(vm);

	if (vm->dynamicLoadBuffers == nullptr) {
		throwNewInternalError(env, const_cast<char *>("Dynamic loader is unavailable"));
		return nullptr;
	}
	dynFuncs = vm->dynamicLoadBuffers;

	if (classBytes == nullptr) {
		throwNewNullPointerException(env, nullptr);
		return nullptr;
	}

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if (nullptr != className) {
		j9object_t classNameObject = J9_JNI_UNWRAP_REFERENCE(className);
		utf8Name = reinterpret_cast<U_8 *>(vmFuncs->copyStringToUTF8WithMemAlloc(
		        currentThread, classNameObject, J9_STR_NULL_TERMINATE_RESULT, "", 0, utf8NameStackBuffer,
		        J9VM_PACKAGE_NAME_BUFFER_LENGTH, &utf8Length));
		if (nullptr == utf8Name) {
			vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
			goto done;
		}

		if (CLASSNAME_INVALID
		    == vmFuncs->verifyQualifiedName(currentThread, utf8Name, utf8Length, CLASSNAME_VALID_NON_ARRARY)) {
			vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGNOCLASSDEFFOUNDERROR,
			                             reinterpret_cast<UDATA *>(*reinterpret_cast<j9object_t *>(className)));
			goto done;
		}
	}

	classLoader = J9VMJAVALANGCLASSLOADER_VMREF(currentThread, J9_JNI_UNWRAP_REFERENCE(classLoaderObject));

retry:

	threadEnv->monitor_enter(vm->classTableMutex);

	if (vmFuncs->hashClassTableAt(classLoader, utf8Name, utf8Length) != nullptr) {
		/* Bad, we have already defined this class - fail */
		threadEnv->monitor_exit(vm->classTableMutex);
		vmFuncs->setCurrentExceptionNLSWithArgs(currentThread, J9NLS_JCL_DUPLICATE_CLASS_DEFINITION,
		                                        J9VMCONSTANTPOOL_JAVALANGLINKAGEERROR, utf8Length, utf8Name);
		goto done;
	}

	tempClassBytes = reinterpret_cast<U_8 *>(classBytes);
	tempLength = length;

	/* Check for romClass cookie, it indicates that we are  defining a class out of a JXE not from class bytes */

	loadedClass = vmFuncs->romClassLoadFromCookie(currentThread,
												  reinterpret_cast<U_8 *>(utf8Name),
												  utf8Length,
												  reinterpret_cast<U_8 *>(classBytes),
	                                              static_cast<UDATA>(length));

	if (nullptr != loadedClass) {
		/* An existing ROMClass is found in the shared class cache.
		 * If -Xshareclasses:enableBCI is present, need to give VM a chance to trigger ClassFileLoadHook event.
		 */
		if ((nullptr == vm->sharedClassConfig) || (0 == vm->sharedClassConfig->isBCIEnabled(vm))) {
			clazz = vm->internalVMFunctions->internalCreateRAMClassFromROMClass(
			        currentThread, classLoader, loadedClass, 0, nullptr,
			        protectionDomain ? *reinterpret_cast<j9object_t *>(protectionDomain) : nullptr, nullptr, J9_CP_INDEX_NONE,
			        LOAD_LOCATION_UNKNOWN, nullptr, nullptr);
			/* Done if a class was found or and exception is pending, otherwise try to define the bytes */
			if ((clazz != nullptr) || (currentThread->currentException != nullptr)) {
				goto done;
			}
			loadedClass = nullptr;
		} else {
			tempClassBytes = J9ROMCLASS_INTERMEDIATECLASSDATA(loadedClass);
			tempLength = loadedClass->intermediateClassDataLength;
			options |= J9_FINDCLASS_FLAG_SHRC_ROMCLASS_EXISTS;
		}
	}

	/* The defineClass helper requires you hold the class table mutex and releases it for you */

	clazz = dynFuncs->internalDefineClassFunction(
	        currentThread, utf8Name, utf8Length, tempClassBytes, static_cast<UDATA>(tempLength), nullptr, classLoader,
	        protectionDomain ? *reinterpret_cast<j9object_t *>(protectionDomain) : nullptr,
	        options | J9_FINDCLASS_FLAG_THROW_ON_FAIL | J9_FINDCLASS_FLAG_NO_CHECK_FOR_EXISTING_CLASS, loadedClass,
	        nullptr, &localBuffer);

	/* If OutOfMemory, try a GC to free up some memory */

	if (currentThread->privateFlags & J9_PRIVATE_FLAGS_CLOAD_NO_MEM) {
		if (!retried) {
			/*Trc_VM_internalFindClass_gcAndRetry(vmThread);*/
			currentThread->javaVM->memoryManagerFunctions->j9gc_modron_global_collect_with_overrides(
			        currentThread, J9MMCONSTANT_EXPLICIT_GC_NATIVE_OUT_OF_MEMORY);
			retried = TRUE;
			goto retry;
		}
		vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
	}

done:
	if ((clazz == nullptr) && (currentThread->currentException == nullptr)) {
		/* should not get here -- throw the default exception just in case */
		vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGCLASSFORMATERROR, nullptr);
	}

	result = static_cast<jclass>(vmFuncs->j9jni_createLocalRef(env, J9VM_J9CLASS_TO_HEAPCLASS(clazz)));

	vmFuncs->internalExitVMToJNI(currentThread);

	if (reinterpret_cast<U_8 *>(utf8NameStackBuffer) != utf8Name) {
		j9mem_free_memory(utf8Name);
	}

	return result;
}

jobject JNICALL
JVM_Bind(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_Bind() stubbed!");
	return nullptr;
}

#if JAVA_SPEC_VERSION < 17

jobject JNICALL
JVM_DTraceActivate(jint arg0, jint arg1, jint arg2, jint arg3, jint arg4)
{
	assert(!"JVM_DTraceActivate() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_DTraceDispose(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_DTraceDispose() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_DTraceGetVersion(jint arg0)
{
	assert(!"JVM_DTraceGetVersion() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_DTraceIsProbeEnabled(jint arg0, jint arg1)
{
	assert(!"JVM_DTraceIsProbeEnabled() stubbed!");
	return nullptr;
}

jboolean JNICALL
JVM_DTraceIsSupported(JNIEnv *env)
{
	return JNI_FALSE;
}

#endif /* JAVA_SPEC_VERSION < 17 */

jobject JNICALL
JVM_DefineClass(jint arg0, jint arg1, jint arg2, jint arg3, jint arg4, jint arg5)
{
	assert(!"JVM_DefineClass() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_DefineClassWithSourceCond(jint arg0, jint arg1, jint arg2, jint arg3, jint arg4, jint arg5, jint arg6, jint arg7)
{
	assert(!"JVM_DefineClassWithSourceCond() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_EnqueueOperation(jint arg0, jint arg1, jint arg2, jint arg3, jint arg4)
{
	assert(!"A HotSpot VM Attach API is attempting to connect to an OpenJ9 VM. This is not supported.");
	return nullptr;
}

jobject JNICALL
JVM_GetCPFieldNameUTF(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_GetCPFieldNameUTF() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetClassConstructor(jint arg0, jint arg1, jint arg2, jint arg3)
{
	assert(!"JVM_GetClassConstructor() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetClassConstructors(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_GetClassConstructors() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetClassField(jint arg0, jint arg1, jint arg2, jint arg3)
{
	assert(!"JVM_GetClassField() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetClassFields(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_GetClassFields() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetClassMethod(jint arg0, jint arg1, jint arg2, jint arg3, jint arg4)
{
	assert(!"JVM_GetClassMethod() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetClassMethods(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_GetClassMethods() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetField(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_GetField() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetFieldAnnotations(jint arg0, jint arg1)
{
	assert(!"JVM_GetFieldAnnotations() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetMethodAnnotations(jint arg0, jint arg1)
{
	assert(!"JVM_GetMethodAnnotations() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetMethodDefaultAnnotationValue(jint arg0, jint arg1)
{
	assert(!"JVM_GetMethodDefaultAnnotationValue() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetMethodParameterAnnotations(jint arg0, jint arg1)
{
	assert(!"JVM_GetMethodParameterAnnotations() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_GetPrimitiveField(jint arg0, jint arg1, jint arg2, jint arg3)
{
	assert(!"JVM_GetPrimitiveField() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_InitializeCompiler(jint arg0, jint arg1)
{
	assert(!"JVM_InitializeCompiler() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_IsSilentCompiler(jint arg0, jint arg1)
{
	assert(!"JVM_IsSilentCompiler() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_LoadClass0(jint arg0, jint arg1, jint arg2, jint arg3)
{
	assert(!"JVM_LoadClass0() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_NewInstance(jint arg0, jint arg1)
{
	assert(!"JVM_NewInstance() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_PrintStackTrace(jint arg0, jint arg1, jint arg2)
{
	assert(!"JVM_PrintStackTrace() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_SetField(jint arg0, jint arg1, jint arg2, jint arg3)
{
	assert(!"JVM_SetField() stubbed!");
	return nullptr;
}

jobject JNICALL
JVM_SetPrimitiveField(jint arg0, jint arg1, jint arg2, jint arg3, jint arg4, jint arg5)
{
	assert(!"JVM_SetPrimitiveField() stubbed!");
	return nullptr;
}

void JNICALL
JVM_SetNativeThreadName(jint arg0, jobject arg1, jstring arg2)
{
	assert(!"JVM_SetNativeThreadName() stubbed!");
}

// end of j7vmi.c

jbyteArray JNICALL
JVM_GetClassTypeAnnotations(JNIEnv *env, jclass jlClass)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetClassTypeAnnotations(env, jlClass);
}

jbyteArray JNICALL
JVM_GetFieldTypeAnnotations(JNIEnv *env, jobject jlrField)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetFieldTypeAnnotations(env, jlrField);
}

jobjectArray JNICALL
JVM_GetMethodParameters(JNIEnv *env, jobject jlrExecutable)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetMethodParameters(env, jlrExecutable);
}

jbyteArray JNICALL
JVM_GetMethodTypeAnnotations(JNIEnv *env, jobject jlrMethod)
{
	ENSURE_VMI();
	return g_VMI->JVM_GetMethodTypeAnnotations(env, jlrMethod);
}

jboolean JNICALL
JVM_IsVMGeneratedMethodIx(JNIEnv *env, jclass cb, jint index)
{
	assert(!"JVM_IsVMGeneratedMethodIx unimplemented"); /* Jazz 63527: Stub in APIs for Java 8 */
	return FALSE;
}

/**
 * Returns platform specific temporary directory used by the system.
 * Same as getTmpDir() defined in jcl/unix/syshelp.c and jcl/win32/syshelp.c.
 *
 * @param [in] env Pointer to JNI environment.
 *
 * @return String object representing the platform specific temporary directory.
 */
jstring JNICALL
JVM_GetTemporaryDirectory(JNIEnv *env)
{
	PORT_ACCESS_FROM_ENV(env);
	jstring result = nullptr;
	IDATA size = j9sysinfo_get_tmp(nullptr, 0, TRUE);
	if (0 <= size) {
		char *buffer = reinterpret_cast<char *>(j9mem_allocate_memory(size, OMRMEM_CATEGORY_VM));
		if (nullptr == buffer) {
			return nullptr;
		}
		if (0 == j9sysinfo_get_tmp(buffer, size, TRUE)) {
			result = env->NewStringUTF(buffer);
		}

		j9mem_free_memory(buffer);
	}

	return result;
}

/**
 * Copies memory from one place to another, endian flipping the data.
 *
 * Implementation of native java.nio.Bits.copySwapMemory0(). The single java caller
 * has ensured all of the parameters are valid.
 *
 * @param [in] env Pointer to JNI environment
 * @param [in] srcObj Source primitive array (nullptr means srcOffset represents native memory)
 * @param [in] srcOffset Offset in source array / address in native memory
 * @param [in] dstObj Destination primitive array (nullptr means dstOffset represents native memory)
 * @param [in] dstOffset Offset in destination array / address in native memory
 * @param [in] size Number of bytes to copy
 * @param [in] elemSize Size of elements to copy and flip
 *
 * elemSize = 2 means byte order 1,2 becomes 2,1
 * elemSize = 4 means byte order 1,2,3,4 becomes 4,3,2,1
 * elemSize = 8 means byte order 1,2,3,4,5,6,7,8 becomes 8,7,6,5,4,3,2,1
 */
void JNICALL
JVM_CopySwapMemory(JNIEnv *env,
                   jobject srcObj,
                   jlong srcOffset,
                   jobject dstObj,
                   jlong dstOffset,
                   jlong size,
                   jlong elemSize)
{
	U_8 *srcBytes = nullptr;
	U_8 *dstBytes = nullptr;
	U_8 *dstAddr = nullptr;
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	if (nullptr != srcObj) {
		srcBytes = reinterpret_cast<U_8 *>(env->GetPrimitiveArrayCritical(static_cast<jarray>(srcObj), nullptr));
		/* The java caller has added Unsafe.arrayBaseOffset() to the offset. Remove it
		 * here as GetPrimitiveArrayCritical returns a pointer to the first element.
		 */
		srcOffset -= J9VMTHREAD_CONTIGUOUS_INDEXABLE_HEADER_SIZE(currentThread);
	}
	if (nullptr != dstObj) {
		dstBytes = reinterpret_cast<U_8 *>(env->GetPrimitiveArrayCritical(static_cast<jarray>(dstObj), nullptr));
		dstAddr = dstBytes;
		/* The java caller has added Unsafe.arrayBaseOffset() to the offset. Remove it
		 * here as GetPrimitiveArrayCritical returns a pointer to the first element.
		 */
		dstOffset -= J9VMTHREAD_CONTIGUOUS_INDEXABLE_HEADER_SIZE(currentThread);
	}
	dstAddr += static_cast<UDATA>(dstOffset);
	/* First copy the bytes unmodified to the new location (memmove handles the overlap case) */
	memmove(dstAddr, srcBytes + static_cast<UDATA>(srcOffset), static_cast<size_t>(size));
	/* Now flip each element in the destination */
	switch (elemSize) {
	case 2: {
		jlong elemCount = size / 2;
		while (0 != elemCount) {
			U_8 temp = dstAddr[0];
			dstAddr[0] = dstAddr[1];
			dstAddr[1] = temp;
			dstAddr += 2;
			elemCount -= 1;
		}
		break;
	}
	case 4: {
		jlong elemCount = size / 4;
		while (0 != elemCount) {
			U_8 temp = dstAddr[0];
			dstAddr[0] = dstAddr[3];
			dstAddr[3] = temp;
			temp = dstAddr[1];
			dstAddr[1] = dstAddr[2];
			dstAddr[2] = temp;
			dstAddr += 4;
			elemCount -= 1;
		}
		break;
	}
	default /* 8 */: {
		jlong elemCount = size / 8;
		while (0 != elemCount) {
			U_8 temp = dstAddr[0];
			dstAddr[0] = dstAddr[7];
			dstAddr[7] = temp;
			temp = dstAddr[1];
			dstAddr[1] = dstAddr[6];
			dstAddr[6] = temp;
			temp = dstAddr[2];
			dstAddr[2] = dstAddr[5];
			dstAddr[5] = temp;
			temp = dstAddr[3];
			dstAddr[3] = dstAddr[4];
			dstAddr[4] = temp;
			dstAddr += 8;
			elemCount -= 1;
		}
		break;
	}
	}
	if (nullptr != srcObj) {
		env->ReleasePrimitiveArrayCritical(static_cast<jarray>(srcObj), srcBytes, JNI_ABORT);
	}
	if (nullptr != dstObj) {
		env->ReleasePrimitiveArrayCritical(static_cast<jarray>(dstObj), dstBytes, 0);
	}
}

// end of j8vmi.c

#if JAVA_SPEC_VERSION >= 11
/* These come from jvm.c */
extern IDATA (*f_monitorEnter)(omrthread_monitor_t monitor);
extern IDATA (*f_monitorExit)(omrthread_monitor_t monitor);

static UDATA
hashTableAtPut(J9HashTable *table, void *value, bool collisionIsFailure)
{
	UDATA retval = HASHTABLE_ATPUT_GENERAL_FAILURE;
	void *node = nullptr;

	/* hashTableAdd() will return the conflicting entry found in the hash in case of collision. Therefore,
	 * we can't use it to figure out whether our value is already found in the has
	 */
	node = hashTableFind(table, value);

	/* If no conflicting entry is found ... */
	if (nullptr == node) {
		node = hashTableAdd(table, value);

		if (nullptr != node) {
			retval = HASHTABLE_ATPUT_SUCCESS;
		}
	} else if (collisionIsFailure) {
		retval = HASHTABLE_ATPUT_COLLISION_FAILURE;
	} else {
		Trc_MODULE_hashTableAtPut(table, value, node);
		retval = HASHTABLE_ATPUT_SUCCESS;
	}

	return retval;
}

static UDATA
hashPackageTableDelete(J9VMThread *currentThread, J9ClassLoader *classLoader, const char *packageName)
{
	J9HashTable *table = classLoader->packageHashTable;
	J9Package package = {0};
	J9Package *packagePtr = &package;
	PORT_ACCESS_FROM_VMC(currentThread);
	UDATA rc = 1; /* hashTableRemove failure code */
	U_8 buf[J9VM_PACKAGE_NAME_BUFFER_LENGTH];

	if (!addUTFNameToPackage(currentThread, packagePtr, packageName, buf, J9VM_PACKAGE_NAME_BUFFER_LENGTH)) {
		return rc;
	}

	rc = hashTableRemove(table, &package);

	if (reinterpret_cast<U_8 *>(package.packageName) != reinterpret_cast<U_8 *>(buf)) {
		j9mem_free_memory(package.packageName);
	}
	return rc;
}

/**
 * A modularity helper method to throw an exception according to the incoming error code.
 *
 * @param[in] currentThread the current J9VMThread
 * @param[in] errCode a modularity error code
 *
 * @return Void
 */
static void
throwExceptionHelper(J9VMThread *currentThread, UDATA errCode)
{
	if (ERRCODE_SUCCESS != errCode) {
		OMRPORT_ACCESS_FROM_J9VMTHREAD(currentThread);
		U_32 moduleName = 0;
		U_32 messageNumber = 0;
		const char *msg = nullptr;

		switch (errCode) {
		case ERRCODE_GENERAL_FAILURE:
			moduleName = J9NLS_VM_MODULARITY_GENERAL_FAILURE__MODULE;
			messageNumber = J9NLS_VM_MODULARITY_GENERAL_FAILURE__ID;
			break;
		case ERRCODE_PACKAGE_ALREADY_DEFINED:
			moduleName = J9NLS_VM_MODULARITY_PACKAGE_ALREADY_DEFINED__MODULE;
			messageNumber = J9NLS_VM_MODULARITY_PACKAGE_ALREADY_DEFINED__ID;
			break;
		case ERRCODE_MODULE_ALREADY_DEFINED:
			moduleName = J9NLS_VM_MODULARITY_MODULE_ALREADY_DEFINED__MODULE;
			messageNumber = J9NLS_VM_MODULARITY_MODULE_ALREADY_DEFINED__ID;
			break;
		case ERRCODE_HASHTABLE_OPERATION_FAILED:
			moduleName = J9NLS_VM_MODULARITY_HASH_OPERATION_FAILED__MODULE;
			messageNumber = J9NLS_VM_MODULARITY_HASH_OPERATION_FAILED__ID;
			break;
		case ERRCODE_DUPLICATE_PACKAGE_IN_LIST:
			moduleName = J9NLS_VM_MODULARITY_DUPLICATED_PACKAGE_FOUND__MODULE;
			messageNumber = J9NLS_VM_MODULARITY_DUPLICATED_PACKAGE_FOUND__ID;
			break;
		case ERRCODE_MODULE_WASNT_FOUND:
			moduleName = J9NLS_VM_MODULARITY_MODULE_NOT_FOUND__MODULE;
			messageNumber = J9NLS_VM_MODULARITY_MODULE_NOT_FOUND__ID;
			break;
		case ERRCODE_PACKAGE_WASNT_FOUND:
			moduleName = J9NLS_VM_MODULARITY_PACKAGE_NOT_FOUND__MODULE;
			messageNumber = J9NLS_VM_MODULARITY_PACKAGE_NOT_FOUND__ID;
			break;
		default:
			Assert_SC_unreachable();
			break;
		}
		msg = OMRPORTLIB->nls_lookup_message(OMRPORTLIB,
											 J9NLS_DO_NOT_PRINT_MESSAGE_TAG | J9NLS_DO_NOT_APPEND_NEWLINE,
											 moduleName, messageNumber, nullptr);
		currentThread->javaVM->internalVMFunctions->setCurrentExceptionUTF(
			currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION, msg);
	}
}

static void
freePackage(J9VMThread *currentThread, J9Package *j9package)
{
	if (nullptr != j9package) {
		J9JavaVM *const vm = currentThread->javaVM;
		PORT_ACCESS_FROM_JAVAVM(vm);

		if (nullptr != j9package->exportsHashTable) {
			hashTableFree(j9package->exportsHashTable);
		}
		j9mem_free_memory(j9package->packageName);
		pool_removeElement(vm->modularityPool, j9package);
	}
}

static J9Package *
createPackage(J9VMThread *currentThread, J9Module *fromModule, const char *package)
{
	J9JavaVM *const vm = currentThread->javaVM;
	J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;
	J9Package *retval = nullptr;

	J9ClassLoader *const classLoader = fromModule->classLoader;
	J9Package *j9package = reinterpret_cast<J9Package *>(pool_newElement(vm->modularityPool));

	if (nullptr != j9package) {
		j9package->module = fromModule;
		j9package->classLoader = fromModule->classLoader;
		if (!addUTFNameToPackage(currentThread, j9package, package, nullptr, 0)) {
			freePackage(currentThread, j9package);
			return retval;
		}
		j9package->exportsHashTable =
		        vmFuncs->hashModulePointerTableNew(vm, INITIAL_INTERNAL_MODULE_HASHTABLE_SIZE);
		if (nullptr != j9package->exportsHashTable) {
			retval = j9package;
		}
	}

	/* if we failed to create the package */
	if (nullptr == retval) {
		if (nullptr != j9package) {
			freePackage(currentThread, j9package);
		}
		vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
	}

	return retval;
}

/** @note It assumes moduleObject is guaranteed not to be nullptr
 *  @return Pointer to module's classloader
 */
static J9ClassLoader *
getModuleObjectClassLoader(J9VMThread *currentThread, j9object_t moduleObject)
{
	j9object_t classLoader = J9VMJAVALANGMODULE_LOADER(currentThread, moduleObject);
	if (nullptr == classLoader) {
		return currentThread->javaVM->systemClassLoader;
	}

	J9ClassLoader *loader = J9VMJAVALANGCLASSLOADER_VMREF(currentThread, classLoader);
	if (nullptr == loader) {
		J9JavaVM *const vm = currentThread->javaVM;
		loader = vm->internalVMFunctions->internalAllocateClassLoader(vm, classLoader);
	}
	return loader;
}

/** @throws OutOfMemory exception if memory cannot be allocated */
static J9Module *
createModule(J9VMThread *currentThread, j9object_t moduleObject, J9ClassLoader *classLoader, j9object_t moduleName)
{
	J9JavaVM *const vm = currentThread->javaVM;
	J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;
	J9Module *j9mod = nullptr;
	J9Module *retval = nullptr;

	if (J9_ARE_ALL_BITS_SET(vm->runtimeFlags, J9_RUNTIME_JAVA_BASE_MODULE_CREATED)) {
		j9mod = reinterpret_cast<J9Module *>(pool_newElement(vm->modularityPool));
	} else {
		if (nullptr == moduleName) {
			/* moduleName is passed as nullptr for the unnamed module for bootloader created by JVM_SetBootLoaderUnnamedModule() */
			j9mod = vm->unamedModuleForSystemLoader;
		} else {
			j9mod = vm->javaBaseModule;
			j9mod->isLoose = TRUE;
		}
	}
	if (nullptr != j9mod) {
		j9mod->moduleName = moduleName;

		j9mod->readAccessHashTable =
		        vmFuncs->hashModulePointerTableNew(vm, INITIAL_INTERNAL_MODULE_HASHTABLE_SIZE);

		if (nullptr != j9mod->readAccessHashTable) {
			j9mod->classLoader = classLoader;
			/* The GC is expected to update pointer below if it moves the object */
			j9mod->moduleObject = moduleObject;

			/* Bind J9Module and module object via the hidden field */
			J9OBJECT_ADDRESS_STORE(currentThread, moduleObject, vm->modulePointerOffset, j9mod);

			retval = j9mod;
		}
	}

	/* If we failed to create the module */
	if (nullptr == retval) {
		if (nullptr != j9mod) {
			vmFuncs->freeJ9Module(vm, j9mod);
		}
		vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
	}

	return retval;
}

static void
freePackageDefinition(J9VMThread *currentThread, J9ClassLoader *classLoader, const char *packageName)
{
	J9Package *j9package = hashPackageTableAt(currentThread, classLoader, packageName);

	if (nullptr != j9package) {
		freePackage(currentThread, j9package);
	}
}

static bool
removePackageDefinition(J9VMThread *currentThread, J9Module *fromModule, const char *packageName)
{
	J9ClassLoader *const classLoader = fromModule->classLoader;

	bool const retval = (0 == hashPackageTableDelete(currentThread, classLoader, packageName));

	freePackageDefinition(currentThread, classLoader, packageName);

	return retval;
}

static void
trcModulesCreationPackage(J9VMThread *currentThread, J9Module *fromModule, const char *package)
{
	PORT_ACCESS_FROM_VMC(currentThread);
	J9InternalVMFunctions const *const vmFuncs = currentThread->javaVM->internalVMFunctions;
	char moduleNameBuf[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
	char *moduleNameUTF = vmFuncs->copyStringToUTF8WithMemAlloc(currentThread, fromModule->moduleName,
	                                                            J9_STR_NULL_TERMINATE_RESULT, "", 0, moduleNameBuf,
	                                                            J9VM_PACKAGE_NAME_BUFFER_LENGTH, nullptr);

	if (nullptr != moduleNameUTF) {
		if (0 == strcmp(moduleNameUTF, JAVA_BASE_MODULE)) {
			Trc_MODULE_createPackage(currentThread, package, "java.base", fromModule);
		} else {
			Trc_MODULE_createPackage(currentThread, package, moduleNameUTF, fromModule);
		}
		if (moduleNameBuf != moduleNameUTF) {
			j9mem_free_memory(moduleNameUTF);
		}
	} else {
		vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
	}
}

static bool
addPackageDefinition(J9VMThread *currentThread, J9Module *fromModule, const char *package)
{
	J9ClassLoader *const classLoader = fromModule->classLoader;

	bool retval = FALSE;

	J9Package *j9package = createPackage(currentThread, fromModule, package);

	if (nullptr != j9package) {
		Trc_MODULE_invokeHashTableAtPut(currentThread, "addPackageDefinition", classLoader,
		                                classLoader->packageHashTable, &j9package, j9package, "true");
		retval = (0 == hashTableAtPut(classLoader->packageHashTable, &j9package, TRUE));
	}

	if (!retval) {
		freePackage(currentThread, j9package);
	} else {
		if (TrcEnabled_Trc_MODULE_createPackage) {
			trcModulesCreationPackage(currentThread, fromModule, package);
		}
	}

	return retval;
}

static void
removeMulPackageDefinitions(J9VMThread *currentThread,
                            J9Module *fromModule,
                            const char *const *packages,
                            U_32 packagesIndex)
{
	bool stopLoop = FALSE;
	U_32 i = packagesIndex;

	while (!stopLoop) {
		const char *packageName = packages[i];

		Assert_SC_true(removePackageDefinition(currentThread, fromModule, packageName));

		stopLoop = (0 == i);
		--i;
	}
}

static UDATA
addMulPackageDefinitions(J9VMThread *currentThread, J9Module *fromModule, const char *const *packages, U_32 numPackages)
{
	UDATA retval = ERRCODE_SUCCESS;

	if (nullptr != packages) {
		U_32 const arrayLength = numPackages;
		if (0 != arrayLength) {
			U_32 i = 0;

			for (i = 0; i < arrayLength; i++) {
				const char *packageName = packages[i];
				if (!addPackageDefinition(currentThread, fromModule, packageName)) {
					J9ClassLoader *const classLoader = fromModule->classLoader;

					if (isPackageDefined(currentThread, classLoader, packageName)) {
						retval = ERRCODE_DUPLICATE_PACKAGE_IN_LIST;
					}
					break;
				}
			}

			/* Remove from the hash table the entries that made through. Note that the last entry (the one we are
			 * processing right now) was the one that failed so we don't need to worry about that one.
			 */
			if (ERRCODE_SUCCESS != retval) {
				if (i > 0) {
					--i;
					removeMulPackageDefinitions(currentThread, fromModule, packages, i);
				}
			}
		}
	}

	return retval;
}

static UDATA
addModuleDefinition(J9VMThread *currentThread,
                    J9Module *fromModule,
                    const char *const *packages,
                    U_32 numPackages,
                    jstring version)
{
	J9ClassLoader *const classLoader = fromModule->classLoader;

	UDATA retval = ERRCODE_GENERAL_FAILURE;
	if (!areNoPackagesDefined(currentThread, classLoader, packages, numPackages)) {
		retval = ERRCODE_PACKAGE_ALREADY_DEFINED;
	} else if (isModuleDefined(currentThread, fromModule)) {
		retval = ERRCODE_MODULE_ALREADY_DEFINED;
	} else {
		retval = addMulPackageDefinitions(currentThread, fromModule, packages, numPackages);
		if (ERRCODE_SUCCESS == retval) {
			bool const success =
			        (0 == hashTableAtPut(classLoader->moduleHashTable, &fromModule, TRUE));
			Trc_MODULE_invokeHashTableAtPut(currentThread, "addModuleDefinition", classLoader,
			                                classLoader->moduleHashTable, &fromModule, fromModule, "true");
			if (nullptr != version) {
				fromModule->version = J9_JNI_UNWRAP_REFERENCE(version);
			}
			if (!success) {
				/* If we failed to add the module to the hash table */
				if (nullptr != packages) {
					removeMulPackageDefinitions(currentThread, fromModule, packages, numPackages);
				}

				retval = ERRCODE_HASHTABLE_OPERATION_FAILED;
			}
		}
	}

	return retval;
}

static bool
isPackageDefined(J9VMThread *currentThread, J9ClassLoader *classLoader, const char *packageName)
{
	J9Package const *target = nullptr;

	target = hashPackageTableAt(currentThread, classLoader, packageName);

	return (nullptr != target);
}

static bool
areNoPackagesDefined(J9VMThread *currentThread,
                     J9ClassLoader *classLoader,
                     const char *const *packages,
                     U_32 numPackages)
{
	bool success = TRUE;
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;

	/*
	 * This check will be ignored for calls to this method that occur before java.base is defined.
	 * Classes are loaded before java.base is created that are added to the classHashTable.
	 * These classes will eventually be fixed up to be part of java.base, but should not be considered duplicate packages
	 * before that happens.
	 */
	bool checkDefinedPackages = J9_ARE_ALL_BITS_SET(vm->runtimeFlags, J9_RUNTIME_JAVA_BASE_MODULE_CREATED);

	if (nullptr != packages) {
		U_32 const arrayLength = numPackages;
		if (0 != arrayLength) {
			U_32 i = 0;
			for (i = 0; success && (i < arrayLength); i++) {
				const char *packageName = packages[i];
				if (checkDefinedPackages && vmFuncs->isAnyClassLoadedFromPackage(
															classLoader,
															reinterpret_cast<U_8 *>(const_cast<char *>(packageName)),
				                                            strlen(packageName))
				) {
					success = FALSE;
				}
			}
		}
	}

	return success;
}

static void
trcModulesAddModuleExportsToAll(J9VMThread *currentThread, J9Module *fromModule, const char *package)
{
	PORT_ACCESS_FROM_VMC(currentThread);
	J9InternalVMFunctions const *const vmFuncs = currentThread->javaVM->internalVMFunctions;
	char fromModuleNameBuf[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
	char *fromModuleNameUTF = vmFuncs->copyStringToUTF8WithMemAlloc(
	        currentThread, fromModule->moduleName, J9_STR_NULL_TERMINATE_RESULT, "", 0, fromModuleNameBuf,
	        J9VM_PACKAGE_NAME_BUFFER_LENGTH, nullptr);
	if (nullptr != fromModuleNameUTF) {
		Trc_MODULE_addModuleExportsToAll(currentThread, package, fromModuleNameUTF, fromModule);
		if (fromModuleNameBuf != fromModuleNameUTF) {
			j9mem_free_memory(fromModuleNameUTF);
		}
	}
}

static UDATA
exportPackageToAll(J9VMThread *currentThread, J9Module *fromModule, const char *package)
{
	UDATA retval = ERRCODE_GENERAL_FAILURE;
	J9Package *const j9package = getPackageDefinition(currentThread, fromModule, package, &retval);
	if (nullptr != j9package) {
		j9package->exportToAll = 1;
		if (TrcEnabled_Trc_MODULE_addModuleExportsToAll) {
			trcModulesAddModuleExportsToAll(currentThread, fromModule, package);
		}
	}

	return retval;
}

static void
trcModulesAddModuleExportsToAllUnnamed(J9VMThread *currentThread, J9Module *fromModule, const char *package)
{
	PORT_ACCESS_FROM_VMC(currentThread);
	J9InternalVMFunctions const *const vmFuncs = currentThread->javaVM->internalVMFunctions;
	char fromModuleNameBuf[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
	char *fromModuleNameUTF = vmFuncs->copyStringToUTF8WithMemAlloc(
	        currentThread, fromModule->moduleName, J9_STR_NULL_TERMINATE_RESULT, "", 0, fromModuleNameBuf,
	        J9VM_PACKAGE_NAME_BUFFER_LENGTH, nullptr);
	if (nullptr != fromModuleNameUTF) {
		Trc_MODULE_addModuleExportsToAllUnnamed(currentThread, package, fromModuleNameUTF, fromModule);
		if (fromModuleNameBuf != fromModuleNameUTF) {
			j9mem_free_memory(fromModuleNameUTF);
		}
	}
}

static UDATA
exportPackageToAllUnamed(J9VMThread *currentThread, J9Module *fromModule, const char *package)
{
	UDATA retval = ERRCODE_GENERAL_FAILURE;
	J9Package *const j9package = getPackageDefinition(currentThread, fromModule, package, &retval);
	if (nullptr != j9package) {
		j9package->exportToAllUnnamed = 1;
		if (TrcEnabled_Trc_MODULE_addModuleExportsToAllUnnamed) {
			trcModulesAddModuleExportsToAllUnnamed(currentThread, fromModule, package);
		}
	}

	return retval;
}

/** @return the J9Module associated with a Module object */
static J9Module *
getJ9Module(J9VMThread *currentThread, jobject module)
{
	J9JavaVM const *const vm = currentThread->javaVM;

	j9object_t modObj = J9_JNI_UNWRAP_REFERENCE(module);

	/* Get J9Module* via the hidden field */
	return reinterpret_cast<J9Module *>(J9OBJECT_ADDRESS_LOAD(currentThread, modObj, vm->modulePointerOffset));
}

static bool
isModuleJavaBase(j9object_t moduleName)
{
	/** @todo compare against string 'java.base' */
	return FALSE;
}

static bool
isModuleNameGood(j9object_t moduleName)
{
	/** @todo implement this */
	return TRUE;
}

static bool
isModuleNameValid(j9object_t moduleName)
{
	bool retval = FALSE;

	if (nullptr != moduleName) {
		retval = TRUE;
		if (!isModuleJavaBase(moduleName)) {
			retval = isModuleNameGood(moduleName);
		}
	}

	return retval;
}

static void
trcModulesAddModuleExports(J9VMThread *currentThread, J9Module *fromModule, const char *package, J9Module *toModule)
{
	PORT_ACCESS_FROM_VMC(currentThread);
	J9InternalVMFunctions const *const vmFuncs = currentThread->javaVM->internalVMFunctions;
	char fromModuleNameBuf[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
	char toModuleNameBuf[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
	char *fromModuleNameUTF = vmFuncs->copyStringToUTF8WithMemAlloc(
	        currentThread, fromModule->moduleName, J9_STR_NULL_TERMINATE_RESULT, "", 0, fromModuleNameBuf,
	        J9VM_PACKAGE_NAME_BUFFER_LENGTH, nullptr);
	char *toModuleNameUTF =
	        vmFuncs->copyStringToUTF8WithMemAlloc(currentThread, toModule->moduleName, J9_STR_NULL_TERMINATE_RESULT,
	                                              "", 0, toModuleNameBuf, J9VM_PACKAGE_NAME_BUFFER_LENGTH, nullptr);
	if ((nullptr != fromModuleNameUTF) && (nullptr != toModuleNameUTF)) {
		Trc_MODULE_addModuleExports(currentThread, package, fromModuleNameUTF, fromModule, toModuleNameUTF,
		                            toModule);
	}
	if (fromModuleNameBuf != fromModuleNameUTF) {
		j9mem_free_memory(fromModuleNameUTF);
	}
	if (toModuleNameBuf != toModuleNameUTF) {
		j9mem_free_memory(toModuleNameUTF);
	}
}

static UDATA
exportPackageToModule(J9VMThread *currentThread, J9Module *fromModule, const char *package, J9Module *toModule)
{
	UDATA retval = ERRCODE_GENERAL_FAILURE;
	J9Package *const j9package = getPackageDefinition(currentThread, fromModule, package, &retval);
	if (nullptr != j9package) {
		if (isModuleDefined(currentThread, toModule)) {
			Trc_MODULE_invokeHashTableAtPut(currentThread, "exportPackageToModule(exportsHashTable)",
			                                j9package, j9package->exportsHashTable, &toModule, toModule,
			                                "false");
			if (0 == hashTableAtPut(j9package->exportsHashTable, &toModule, FALSE)) {
				retval = ERRCODE_SUCCESS;
				/*
				 * Need to keep track of package that is exported to toModule in case toModule gets unloaded
				 * before fromModule. An unloaded module in packageHashtable will result in a crash when doing hashtable lookups.
				 * We use this hashtable to remove instances of toModule in packages when unloading toModule.
				 * We only need to worry about modules in different layers as modules in the same layer are unloaded
				 * at the same time.
				 */
				if (nullptr == toModule->removeExportsHashTable) {
					J9JavaVM *vm = currentThread->javaVM;
					toModule->removeExportsHashTable = vm->internalVMFunctions->hashPackageTableNew(
					        vm, INITIAL_INTERNAL_PACKAGE_HASHTABLE_SIZE);
				}
				if (nullptr != toModule->removeExportsHashTable) {
					Trc_MODULE_invokeHashTableAtPut(currentThread,
					                                "exportPackageToModule(removeExportsHashTable)",
					                                toModule, toModule->removeExportsHashTable,
					                                &j9package, j9package, "false");
					if (0
					    != hashTableAtPut(toModule->removeExportsHashTable, const_cast<J9Package **>(&j9package),
					                      FALSE)) {
						retval = ERRCODE_HASHTABLE_OPERATION_FAILED;
					}
				} else {
					retval = ERRCODE_HASHTABLE_OPERATION_FAILED;
				}
			} else {
				retval = ERRCODE_HASHTABLE_OPERATION_FAILED;
			}
		} else {
			retval = ERRCODE_MODULE_WASNT_FOUND;
		}
	}
	if (ERRCODE_SUCCESS == retval && TrcEnabled_Trc_MODULE_addModuleExports) {
		trcModulesAddModuleExports(currentThread, fromModule, package, toModule);
	}

	return retval;
}

static UDATA
allowReadAccessToModule(J9VMThread *currentThread, J9Module *fromModule, J9Module *toModule)
{
	UDATA retval = ERRCODE_MODULE_WASNT_FOUND;

	if (isModuleDefined(currentThread, fromModule)) {
		J9JavaVM *vm = currentThread->javaVM;

		if (J9_IS_J9MODULE_UNNAMED(vm, toModule)) {
			fromModule->isLoose = TRUE;
			retval = ERRCODE_SUCCESS;
		} else if (isModuleDefined(currentThread, toModule)) {
			bool success = FALSE;
			Trc_MODULE_invokeHashTableAtPut(currentThread, "allowReadAccessToModule(readAccessHashTable)",
			                                toModule, toModule->readAccessHashTable, &fromModule,
			                                fromModule, "false");
			if (0 == hashTableAtPut(toModule->readAccessHashTable, &fromModule, FALSE)) {
				success = TRUE;
				/*
				 * Need to keep track of toModule that can read fromModule in case fromModule gets unloaded
				 * before toModule. An unloaded module in toModule will result in a crash when doing hashtable lookups.
				 * We use removeAccessHashTable to remove instances of fromModule in toModules when unloading fromModule.
				 * We only need to worry about modules in different layers as modules in the same layer are unloaded
				 * at the same time.
				 */
				if (nullptr == fromModule->removeAccessHashTable) {
					fromModule->removeAccessHashTable =
					        vm->internalVMFunctions->hashModulePointerTableNew(
					                vm, INITIAL_INTERNAL_MODULE_HASHTABLE_SIZE);
				}
				if (nullptr != fromModule->removeAccessHashTable) {
					Trc_MODULE_invokeHashTableAtPut(
					        currentThread, "allowReadAccessToModule(removeAccessHashTable)",
					        fromModule, fromModule->removeAccessHashTable, &toModule, toModule,
					        "false");
					if (0
					    != hashTableAtPut(fromModule->removeAccessHashTable, &toModule,
					                      FALSE)) {
						success = FALSE;
					}
				} else {
					retval = ERRCODE_HASHTABLE_OPERATION_FAILED;
				}
			}

			if (success) {
				retval = ERRCODE_SUCCESS;
			} else {

				retval = ERRCODE_HASHTABLE_OPERATION_FAILED;
			}
		}
	}

	return retval;
}

/**
 * Define a module containing the specified packages. It will create the module record in the  ClassLoader's module hash table and
 * create package records in the class loader's package hash table if necessary.
 *
 * @throws NullPointerExceptions a if module is null.
 * @throws IllegalArgumentExceptions if
 *     - Class loader already has a module with that name
 *     - Class loader has already defined types for any of the module's packages
 *     - Module_name is 'java.base'
 *     - Module_name is syntactically bad
 *     - Packages contains an illegal package name
 *     - Packages contains a duplicate package name
 *     - A package already exists in another module for this class loader
 *     - Class loader is not a subclass of java.lang.ClassLoader
 *     - Module is an unnamed module
 * @throws LayerInstantiationException if a module with name 'java.base' is defined by non-bootstrap classloader.
 *
 * @return If successful, returns a java.lang.reflect.Module object. Otherwise, returns nullptr.
 */
jobject JNICALL
#if JAVA_SPEC_VERSION >= 15
JVM_DefineModule(JNIEnv *env,
                 jobject module,
                 jboolean isOpen,
                 jstring version,
                 jstring location,
                 jobjectArray packageArray)
#else
JVM_DefineModule(JNIEnv *env,
                 jobject module,
                 jboolean isOpen,
                 jstring version,
                 jstring location,
                 const char *const *packages,
                 jsize numPackages)
#endif /* JAVA_SPEC_VERSION >= 15 */
{
	J9VMThread *const currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;
#if JAVA_SPEC_VERSION >= 15
	bool oom = FALSE;
	jsize numPackages = 0;
	UDATA packagesNumBytes = 0;
	const char **packages = nullptr;
	PORT_ACCESS_FROM_ENV(env);
#endif /* JAVA_SPEC_VERSION >= 15 */

	vmFuncs->internalEnterVMFromJNI(currentThread);
	f_monitorEnter(vm->classLoaderModuleAndLocationMutex);

#if JAVA_SPEC_VERSION >= 15
	if (nullptr != packageArray) {
		numPackages = J9INDEXABLEOBJECT_SIZE(currentThread, J9_JNI_UNWRAP_REFERENCE(packageArray));
		packagesNumBytes = sizeof(char *) * numPackages;
	} else {
		vmFuncs->setCurrentExceptionNLS(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION,
		                                J9NLS_VM_PACKAGES_IS_NULL);
		goto done;
	}
	packages = static_cast<const char **>(j9mem_allocate_memory(packagesNumBytes, OMRMEM_CATEGORY_VM));
	if (nullptr != packages) {
		jsize pkgIndex = 0;
		memset(packages, 0, packagesNumBytes);
		for (pkgIndex = 0; pkgIndex < numPackages; pkgIndex++) {
			j9array_t array = reinterpret_cast<j9array_t>(J9_JNI_UNWRAP_REFERENCE(packageArray));
			j9object_t stringObject = J9JAVAARRAYOFOBJECT_LOAD(currentThread, array, pkgIndex);
			if (nullptr != stringObject) {
				UDATA utfLength = vmFuncs->getStringUTF8Length(currentThread, stringObject) + 1;
				char *packageName = reinterpret_cast<char *>(j9mem_allocate_memory(utfLength, OMRMEM_CATEGORY_VM));
				if (nullptr == packageName) {
					oom = TRUE;
					break;
				}
				vmFuncs->copyStringToUTF8Helper(currentThread, stringObject,
				                                J9_STR_NULL_TERMINATE_RESULT | J9_STR_XLAT, 0,
				                                J9VMJAVALANGSTRING_LENGTH(currentThread, stringObject),
				                                reinterpret_cast<U_8 *>(packageName), utfLength);
				packages[pkgIndex] = packageName;
			} else {
				vmFuncs->setCurrentExceptionNLS(currentThread,
				                                J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION,
				                                J9NLS_VM_PACKAGE_IS_NULL);
				goto done;
			}
		}
	}
	if ((nullptr == packages) || oom) {
		vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
		goto done;
	}
#endif /* JAVA_SPEC_VERSION >= 15 */

	if (nullptr == module) {
		vmFuncs->setCurrentExceptionNLS(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION,
		                                J9NLS_VM_MODULE_IS_NULL);
	} else {
		j9object_t modObj = J9_JNI_UNWRAP_REFERENCE(module);
		J9ClassLoader *systemClassLoader = vm->systemClassLoader;

		J9ClassLoader *const classLoader = getModuleObjectClassLoader(currentThread, modObj);
		j9object_t moduleName = J9VMJAVALANGMODULE_NAME(currentThread, modObj);

		/* extensionClassLoader holds the platform class loader in Java 11+ */
		if ((classLoader != systemClassLoader) && (classLoader != vm->extensionClassLoader)) {
			jsize pkgIndex = 0;
			for (pkgIndex = 0; pkgIndex < numPackages; pkgIndex++) {
				const char *packageName = packages[pkgIndex];
				if (0 == strncmp(packageName, "java", 4)) {
					char nextCh = packageName[4];
					if (('\0' == nextCh) || ('.' == nextCh) || ('/' == nextCh)) {
						vmFuncs->setCurrentExceptionNLS(
						        currentThread,
						        J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
						        J9NLS_VM_ONLY_BOOT_PLATFORM_CLASSLOADER_DEFINE_PKG_JAVA);
						goto done;
					}
				}
			}
		}

		if (nullptr == moduleName) {
			vmFuncs->setCurrentExceptionNLS(currentThread,
			                                J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
			                                J9NLS_VM_MODULE_IS_UNNAMED);
		} else if (!isModuleNameValid(moduleName)) {
			vmFuncs->setCurrentExceptionNLS(currentThread,
			                                J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
			                                J9NLS_VM_MODULE_NAME_IS_INVALID);
		} else if (nullptr == classLoader) {
			/* An exception should be pending if classLoader is null */
			Assert_SC_true(nullptr != currentThread->currentException);
		} else {
			char buf[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
			char *nameUTF = buf;

			PORT_ACCESS_FROM_VMC(currentThread);
			nameUTF = vmFuncs->copyStringToUTF8WithMemAlloc(currentThread, moduleName,
			                                                J9_STR_NULL_TERMINATE_RESULT, "", 0, buf,
			                                                J9VM_PACKAGE_NAME_BUFFER_LENGTH, nullptr);
			if (nullptr == nameUTF) {
				vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
			} else if ((classLoader != systemClassLoader) && (0 == strcmp(nameUTF, JAVA_BASE_MODULE))) {
				vmFuncs->setCurrentExceptionNLS(currentThread,
				                                J9VMCONSTANTPOOL_JAVALANGLAYERINSTANTIATIONEXCEPTION,
				                                J9NLS_VM_ONLY_BOOTCLASSLOADER_LOAD_MODULE_JAVABASE);
			} else {
				J9Module *j9mod = createModule(currentThread, modObj, classLoader, moduleName);
				if (nullptr != j9mod) {
					bool success = FALSE;
					UDATA rc = addModuleDefinition(currentThread, j9mod, packages,
					                               static_cast<U_32>(numPackages), version);
					j9mod->isOpen = isOpen;
					success = (ERRCODE_SUCCESS == rc);
					if (success) {
						/* For "java.base" module setting of jrt URL and patch paths is already done during startup. Avoid doing it here. */
						if (J9_ARE_ALL_BITS_SET(vm->runtimeFlags,
						                        J9_RUNTIME_JAVA_BASE_MODULE_CREATED)) {
							Trc_MODULE_defineModule(currentThread, nameUTF, j9mod);
							if (classLoader == systemClassLoader) {
								success = vmFuncs->setBootLoaderModulePatchPaths(
								        vm, j9mod, nameUTF);
								if (FALSE == success) {
									vmFuncs->setNativeOutOfMemoryError(
									        currentThread, 0, 0);
								} else {
									const char *moduleName = "openj9.sharedclasses";

									if (0 == strcmp(nameUTF, moduleName)) {
										J9VMDllLoadInfo *entry =
										        FIND_DLL_TABLE_ENTRY(
										                J9_SHARED_DLL_NAME);

										if ((nullptr == entry)
										    || (J9_ARE_ALL_BITS_SET(
										            entry->loadFlags,
										            FAILED_TO_LOAD))) {
											j9nls_printf(
											        PORTLIB, J9NLS_WARNING,
											        J9NLS_VM_FAILED_TO_LOAD_MODULE_REQUIRED_DLL,
											        J9_SHARED_DLL_NAME,
											        moduleName);
										}
									}
								}
							}
						} else {
							/* first module; must be "java.base" */
							J9ClassWalkState classWalkState;
							J9Class *clazz = nullptr;

							Assert_SC_true(0 == strcmp(nameUTF, JAVA_BASE_MODULE));

							clazz = vmFuncs->allClassesStartDo(&classWalkState, vm,
							                                   systemClassLoader);
							while (nullptr != clazz) {
								Assert_SC_true(clazz->module == vm->javaBaseModule);
								J9VMJAVALANGCLASS_SET_MODULE(
								        currentThread, clazz->classObject, modObj);
								clazz = vmFuncs->allClassesNextDo(&classWalkState);
							}
							vmFuncs->allClassesEndDo(&classWalkState);

							if (vm->anonClassCount > 0) {
								J9ClassWalkState classWalkStateAnon = {0};
								J9Class *clazzAnon = nullptr;

								Assert_SC_notNull(vm->anonClassLoader);
								clazzAnon = vmFuncs->allClassesStartDo(
								        &classWalkStateAnon, vm, vm->anonClassLoader);
								while (nullptr != clazzAnon) {
									Assert_SC_true(clazzAnon->module
									               == vm->javaBaseModule);
									J9VMJAVALANGCLASS_SET_MODULE(
									        currentThread, clazzAnon->classObject,
									        modObj);
									clazzAnon = vmFuncs->allClassesNextDo(
									        &classWalkStateAnon);
								}
								vmFuncs->allClassesEndDo(&classWalkStateAnon);
							}

#if JAVA_SPEC_VERSION >= 21
							/* vm->unamedModuleForSystemLoader->moduleObject was saved by JVM_SetBootLoaderUnnamedModule */
							{
								j9object_t moduleObject =
								        vm->unamedModuleForSystemLoader->moduleObject;
								Assert_SC_notNull(moduleObject);
								J9VMJAVALANGCLASSLOADER_SET_UNNAMEDMODULE(
								        currentThread,
								        systemClassLoader->classLoaderObject,
								        moduleObject);
								Trc_MODULE_defineModule_setBootloaderUnnamedModule(
								        currentThread);
							}
#endif /* JAVA_SPEC_VERSION >= 21 */
							vm->runtimeFlags |= J9_RUNTIME_JAVA_BASE_MODULE_CREATED;
							Trc_MODULE_defineModule(currentThread, "java.base", j9mod);
						}
						TRIGGER_J9HOOK_VM_MODULE_LOAD(vm->hookInterface, currentThread, j9mod);
					} else {
						throwExceptionHelper(currentThread, rc);
					}
					if (FALSE == success) {
						vmFuncs->freeJ9Module(vm, j9mod);
						Assert_SC_true(nullptr != currentThread->currentException);
					}
				}
			}
			if (nameUTF != buf) {
				j9mem_free_memory(nameUTF);
			}
		}
	}

done:
#if JAVA_SPEC_VERSION >= 15
	if (nullptr != packages) {
		jsize pkgIndex = 0;
		for (pkgIndex = 0; pkgIndex < numPackages; pkgIndex++) {
			const char *packageName = packages[pkgIndex];
			j9mem_free_memory(const_cast<char *>(packageName));
		}
		j9mem_free_memory(packages);
	}
#endif /* JAVA_SPEC_VERSION >= 15 */

	f_monitorExit(vm->classLoaderModuleAndLocationMutex);
	vmFuncs->internalExitVMToJNI(currentThread);

	return module;
}

/**
 * Qualified export of package in fromModule to toModule.
 *
 * @todo the null toModule case is not outlined in the spec but the spec does not specify what to do in this case
 * @throws NullPointerExceptions a if toModule is null.
 * @throws IllegalArgumentExceptions if
 * 1) Module fromModule does not exist
 * 2) Module toModule is not null and does not exist
 * 3) Package is not syntactically correct
 * 4) Package is not defined for fromModule's class loader
 * 5) Package is not in module fromModule.
 */
#if JAVA_SPEC_VERSION >= 15
void JNICALL
JVM_AddModuleExports(JNIEnv *env, jobject fromModule, jstring packageObj, jobject toModule)
#else
void JNICALL
JVM_AddModuleExports(JNIEnv *env, jobject fromModule, const char *package, jobject toModule)
#endif /* JAVA_SPEC_VERSION >= 15 */
{
	J9VMThread *const currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM const *const vm = currentThread->javaVM;
	J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;
#if JAVA_SPEC_VERSION >= 15
	const char *package = nullptr;
	PORT_ACCESS_FROM_ENV(env);
#endif /* JAVA_SPEC_VERSION >= 15 */

	vmFuncs->internalEnterVMFromJNI(currentThread);
	f_monitorEnter(vm->classLoaderModuleAndLocationMutex);

#if JAVA_SPEC_VERSION >= 15
	if (nullptr != packageObj) {
		j9object_t stringObject = J9_JNI_UNWRAP_REFERENCE(packageObj);
		UDATA utfLength = vmFuncs->getStringUTF8Length(currentThread, stringObject) + 1;
		char *packageName = reinterpret_cast<char *>(j9mem_allocate_memory(utfLength, OMRMEM_CATEGORY_VM));
		if (nullptr == packageName) {
			vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
			goto done;
		}
		vmFuncs->copyStringToUTF8Helper(currentThread, stringObject, J9_STR_NULL_TERMINATE_RESULT | J9_STR_XLAT,
		                                0, J9VMJAVALANGSTRING_LENGTH(currentThread, stringObject),
		                                reinterpret_cast<U_8 *>(packageName), utfLength);
		package = packageName;
	} else {
		vmFuncs->setCurrentExceptionNLS(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION,
		                                J9NLS_VM_PACKAGE_IS_NULL);
		goto done;
	}
#endif /* JAVA_SPEC_VERSION >= 15 */

	if (nullptr == toModule) {
		vmFuncs->setCurrentExceptionUTF(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION,
		                                "module is null");
	} else {
		UDATA rc = ERRCODE_GENERAL_FAILURE;
		J9Module *const j9FromMod = getJ9Module(currentThread, fromModule);
		J9Module *const j9ToMod = getJ9Module(currentThread, toModule);

		if (isModuleUnnamed(currentThread, J9_JNI_UNWRAP_REFERENCE(toModule))) {
			rc = exportPackageToAllUnamed(currentThread, j9FromMod, package);
		} else {
			rc = exportPackageToModule(currentThread, j9FromMod, package, j9ToMod);
		}

		if (ERRCODE_SUCCESS != rc) {
			throwExceptionHelper(currentThread, rc);
		}
	}

#if JAVA_SPEC_VERSION >= 15
done:
	if (nullptr != package) {
		j9mem_free_memory(const_cast<char *>(package));
	}
#endif /* JAVA_SPEC_VERSION >= 15 */

	f_monitorExit(vm->classLoaderModuleAndLocationMutex);
	vmFuncs->internalExitVMToJNI(currentThread);
}

/**
 * Unqualified export of package in fromModule
 *
 * @throws IllegalArgumentExceptions if
 * 1) Module fromModule does not exist
 * 2) Package is not syntactically correct
 * 3) Package is not defined for fromModule's class loader
 * 4) Package is not in module fromModule.
 */
#if JAVA_SPEC_VERSION >= 15
void JNICALL
JVM_AddModuleExportsToAll(JNIEnv *env, jobject fromModule, jstring packageObj)
#else
void JNICALL
JVM_AddModuleExportsToAll(JNIEnv *env, jobject fromModule, const char *package)
#endif /* JAVA_SPEC_VERSION >= 15 */
{
	J9VMThread *const currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM const *const vm = currentThread->javaVM;
	J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;
#if JAVA_SPEC_VERSION >= 15
	const char *package = nullptr;
	PORT_ACCESS_FROM_ENV(env);
#endif /* JAVA_SPEC_VERSION >= 15 */

	vmFuncs->internalEnterVMFromJNI(currentThread);
	f_monitorEnter(vm->classLoaderModuleAndLocationMutex);

#if JAVA_SPEC_VERSION >= 15
	if (nullptr != packageObj) {
		j9object_t stringObject = J9_JNI_UNWRAP_REFERENCE(packageObj);
		UDATA utfLength = vmFuncs->getStringUTF8Length(currentThread, stringObject) + 1;
		char *packageName = reinterpret_cast<char *>(j9mem_allocate_memory(utfLength, OMRMEM_CATEGORY_VM));
		if (nullptr == packageName) {
			vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
			goto done;
		}
		vmFuncs->copyStringToUTF8Helper(currentThread, stringObject, J9_STR_NULL_TERMINATE_RESULT | J9_STR_XLAT,
		                                0, J9VMJAVALANGSTRING_LENGTH(currentThread, stringObject),
		                                reinterpret_cast<U_8 *>(packageName), utfLength);
		package = packageName;
	} else {
		vmFuncs->setCurrentExceptionNLS(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION,
		                                J9NLS_VM_PACKAGE_IS_NULL);
		goto done;
	}
#endif /* JAVA_SPEC_VERSION >= 15 */

	{
		UDATA rc = ERRCODE_GENERAL_FAILURE;

		J9Module *const j9FromMod = getJ9Module(currentThread, fromModule);

		rc = exportPackageToAll(currentThread, j9FromMod, package);

		if (ERRCODE_SUCCESS != rc) {
			throwExceptionHelper(currentThread, rc);
		}
	}

#if JAVA_SPEC_VERSION >= 15
done:
	if (nullptr != package) {
		j9mem_free_memory(const_cast<char *>(package));
	}
#endif /* JAVA_SPEC_VERSION >= 15 */

	f_monitorExit(vm->classLoaderModuleAndLocationMutex);
	vmFuncs->internalExitVMToJNI(currentThread);
}

static void
trcModulesAddReadsModule(J9VMThread *currentThread, jobject toModule, J9Module *j9FromMod, J9Module *j9ToMod)
{
	PORT_ACCESS_FROM_VMC(currentThread);
	J9InternalVMFunctions const *const vmFuncs = currentThread->javaVM->internalVMFunctions;
	char fromModuleNameBuf[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
	char toModuleNameBuf[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
	char *fromModuleNameUTF = vmFuncs->copyStringToUTF8WithMemAlloc(
	        currentThread, j9FromMod->moduleName, J9_STR_NULL_TERMINATE_RESULT, "", 0, fromModuleNameBuf,
	        J9VM_PACKAGE_NAME_BUFFER_LENGTH, nullptr);
	char *toModuleNameUTF = nullptr;

	if (nullptr != j9ToMod) {
		if (nullptr != j9ToMod->moduleName) {
			toModuleNameUTF = vmFuncs->copyStringToUTF8WithMemAlloc(
			        currentThread, j9ToMod->moduleName, J9_STR_NULL_TERMINATE_RESULT, "", 0,
			        toModuleNameBuf, J9VM_PACKAGE_NAME_BUFFER_LENGTH, nullptr);
		} else {
			constexpr const char *UNNAMED_MODULE = "unnamed";
			constexpr size_t UNNAMED_MODULE_LENGTH = strlen(UNNAMED_MODULE);
			PORT_ACCESS_FROM_VMC(currentThread);
			Assert_SC_true(J9VM_PACKAGE_NAME_BUFFER_LENGTH >= UNNAMED_MODULE_LENGTH);
			memcpy(toModuleNameBuf, UNNAMED_MODULE, UNNAMED_MODULE_LENGTH);
			toModuleNameUTF = toModuleNameBuf;
		}
	} else {
		constexpr const char *LOOSE_MODULE = "loose";
		constexpr size_t LOOSE_MODULE_LENGTH = strlen(LOOSE_MODULE);
		PORT_ACCESS_FROM_VMC(currentThread);
		Assert_SC_true(J9VM_PACKAGE_NAME_BUFFER_LENGTH >= LOOSE_MODULE_LENGTH);
		memcpy(toModuleNameBuf, LOOSE_MODULE, LOOSE_MODULE_LENGTH);
		toModuleNameUTF = toModuleNameBuf;
	}
	if ((nullptr != fromModuleNameUTF) && (nullptr != toModuleNameUTF)) {
		Trc_MODULE_addReadsModule(currentThread, fromModuleNameUTF, j9FromMod, toModuleNameUTF, toModule);
	}
	if (fromModuleNameBuf != fromModuleNameUTF) {
		j9mem_free_memory(fromModuleNameUTF);
	}
	if (toModuleNameBuf != toModuleNameUTF) {
		j9mem_free_memory(toModuleNameUTF);
	}
}

/**
 * Add toModule to the list of modules that fromModule can read. If fromModule is the same as toModule, do nothing.
 * If toModule is null then fromModule is marked as a loose module (i.e., fromModule can read all current and future unnamed modules).
 *
 * @throws IllegalArgumentExceptions if
 * 1) if fromModule is null or if modules do not exist.
 */
void JNICALL
JVM_AddReadsModule(JNIEnv *env, jobject fromModule, jobject toModule)
{
	if (fromModule != toModule) {
		J9VMThread *const currentThread = reinterpret_cast<J9VMThread *>(env);
		J9JavaVM const *const vm = currentThread->javaVM;
		J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;

		vmFuncs->internalEnterVMFromJNI(currentThread);
		f_monitorEnter(vm->classLoaderModuleAndLocationMutex);
		{
			UDATA rc = ERRCODE_GENERAL_FAILURE;

			J9Module *const j9FromMod = getJ9Module(currentThread, fromModule);
			J9Module *const j9ToMod = (nullptr != toModule) ? getJ9Module(currentThread, toModule) : nullptr;

			/* Slightly different then check above since above I was dealing with the stack addr */
			if (j9FromMod != j9ToMod) {
				rc = allowReadAccessToModule(currentThread, j9FromMod, j9ToMod);

				if (ERRCODE_SUCCESS != rc) {
					throwExceptionHelper(currentThread, rc);
				} else {
					if (TrcEnabled_Trc_MODULE_addReadsModule) {
						trcModulesAddReadsModule(currentThread, toModule, j9FromMod, j9ToMod);
					}
				}
			}
		}
		f_monitorExit(vm->classLoaderModuleAndLocationMutex);
		vmFuncs->internalExitVMToJNI(currentThread);
	}
}

/**
 * @return TRUE if:
 * 1. askModule can read srcModule or
 * 2. if both are the same module or
 * 3. if askModule is loose and srcModule is null.
 * FALSE otherwise
 *
 * @throws IllegalArgumentExceptions if
 * 1) either askModule or srcModule is not a java.lang.reflect.Module
 */
jboolean JNICALL
JVM_CanReadModule(JNIEnv *env, jobject askModule, jobject srcModule)
{
	J9VMThread *const currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM const *const vm = currentThread->javaVM;
	J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;
	bool canRead = FALSE;

	if (askModule == srcModule) {
		canRead = TRUE;
	} else {
		vmFuncs->internalEnterVMFromJNI(currentThread);
		f_monitorEnter(vm->classLoaderModuleAndLocationMutex);
		{
			UDATA rc = ERRCODE_GENERAL_FAILURE;

			J9Module *const j9FromMod = getJ9Module(currentThread, askModule);
			J9Module *const j9ToMod = getJ9Module(currentThread, srcModule);

			canRead = isAllowedReadAccessToModule(currentThread, j9FromMod, j9ToMod, &rc);

			if (ERRCODE_SUCCESS != rc) {
				throwExceptionHelper(currentThread, rc);
			}
		}
		f_monitorExit(vm->classLoaderModuleAndLocationMutex);
		vmFuncs->internalExitVMToJNI(currentThread);
	}

	return static_cast<jboolean>(canRead);
}

static void
trcModulesAddModulePackage(J9VMThread *currentThread, J9Module *j9mod, const char *package)
{
	PORT_ACCESS_FROM_VMC(currentThread);
	J9InternalVMFunctions const *const vmFuncs = currentThread->javaVM->internalVMFunctions;
	char moduleNameBuf[J9VM_PACKAGE_NAME_BUFFER_LENGTH];
	char *moduleNameUTF = vmFuncs->copyStringToUTF8WithMemAlloc(currentThread, j9mod->moduleName,
	                                                            J9_STR_NULL_TERMINATE_RESULT, "", 0, moduleNameBuf,
	                                                            J9VM_PACKAGE_NAME_BUFFER_LENGTH, nullptr);
	if (nullptr != moduleNameUTF) {
		Trc_MODULE_addModulePackage(currentThread, package, moduleNameUTF, j9mod);
		if (moduleNameBuf != moduleNameUTF) {
			j9mem_free_memory(moduleNameUTF);
		}
	}
}
/**
 * Adds a package to a module
 *
 * @throws IllegalArgumentException is thrown if
 * 1) Module is bad
 * 2) Module is unnamed
 * 3) Package is not syntactically correct
 * 4) Package is already defined for module's class loader.
 */
void JNICALL
JVM_AddModulePackage(JNIEnv *env, jobject module, const char *package)
{
	J9VMThread *const currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM const *const vm = currentThread->javaVM;
	J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;

	vmFuncs->internalEnterVMFromJNI(currentThread);
	f_monitorEnter(vm->classLoaderModuleAndLocationMutex);
	{
		J9Module *const j9mod = getJ9Module(currentThread, module);
		if (addPackageDefinition(currentThread, j9mod, package)) {
			if (TrcEnabled_Trc_MODULE_addModulePackage) {
				trcModulesAddModulePackage(currentThread, j9mod, package);
			}
		} else {
			throwExceptionHelper(currentThread, ERRCODE_GENERAL_FAILURE);
		}
	}
	f_monitorExit(vm->classLoaderModuleAndLocationMutex);
	vmFuncs->internalExitVMToJNI(currentThread);
}

/**
 * Marks the specified package as exported to all unnamed modules.
 *
 * @throws NullPointerExceptions if either module or package is null.
 * @throws IllegalArgumentExceptions if
 * 1) module or package is bad or
 * 2) module is unnamed or
 * 3) package is not in module
 */
#if JAVA_SPEC_VERSION >= 15
void JNICALL
JVM_AddModuleExportsToAllUnnamed(JNIEnv *env, jobject fromModule, jstring packageObj)
#else
void JNICALL
JVM_AddModuleExportsToAllUnnamed(JNIEnv *env, jobject fromModule, const char *package)
#endif /* JAVA_SPEC_VERSION >= 15 */
{
	J9VMThread *const currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM const *const vm = currentThread->javaVM;
	J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;
#if JAVA_SPEC_VERSION >= 15
	const char *package = nullptr;
	PORT_ACCESS_FROM_ENV(env);
#endif /* JAVA_SPEC_VERSION >= 15 */

	vmFuncs->internalEnterVMFromJNI(currentThread);
	f_monitorEnter(vm->classLoaderModuleAndLocationMutex);

#if JAVA_SPEC_VERSION >= 15
	if (nullptr != packageObj) {
		j9object_t stringObject = J9_JNI_UNWRAP_REFERENCE(packageObj);
		UDATA utfLength = vmFuncs->getStringUTF8Length(currentThread, stringObject) + 1;
		char *packageName = reinterpret_cast<char *>(j9mem_allocate_memory(utfLength, OMRMEM_CATEGORY_VM));
		if (nullptr == packageName) {
			vmFuncs->setNativeOutOfMemoryError(currentThread, 0, 0);
			goto done;
		}
		vmFuncs->copyStringToUTF8Helper(currentThread, stringObject, J9_STR_NULL_TERMINATE_RESULT | J9_STR_XLAT,
		                                0, J9VMJAVALANGSTRING_LENGTH(currentThread, stringObject),
		                                reinterpret_cast<U_8 *>(packageName), utfLength);
		package = packageName;
	} else {
		vmFuncs->setCurrentExceptionNLS(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION,
		                                J9NLS_VM_PACKAGE_IS_NULL);
		goto done;
	}
#endif /* JAVA_SPEC_VERSION >= 15 */

	{
		UDATA rc = ERRCODE_GENERAL_FAILURE;

		J9Module *const j9FromMod = getJ9Module(currentThread, fromModule);

		rc = exportPackageToAllUnamed(currentThread, j9FromMod, package);

		if (ERRCODE_SUCCESS != rc) {
			throwExceptionHelper(currentThread, rc);
		}
	}

#if JAVA_SPEC_VERSION >= 15
done:
	if (nullptr != package) {
		j9mem_free_memory(const_cast<char *>(package));
	}
#endif /* JAVA_SPEC_VERSION >= 15 */

	f_monitorExit(vm->classLoaderModuleAndLocationMutex);
	vmFuncs->internalExitVMToJNI(currentThread);
}

jstring JNICALL
JVM_GetSimpleBinaryName(JNIEnv *env, jclass arg1)
{
	assert(!"JVM_GetSimpleBinaryName unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
	return nullptr;
}

void JNICALL
JVM_SetMethodInfo(JNIEnv *env, jobject arg1)
{
	assert(!"JVM_SetMethodInfo unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
}

jint JNICALL
JVM_ConstantPoolGetNameAndTypeRefIndexAt(JNIEnv *env, jobject arg1, jobject arg2, jint arg3)
{
	assert(!"JVM_ConstantPoolGetNameAndTypeRefIndexAt unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
	return -1;
}

jint JNICALL
#if JAVA_SPEC_VERSION >= 22
JVM_MoreStackWalk(JNIEnv *env,
                  jobject arg1,
                  jint arg2,
                  jlong arg3,
                  jint arg4,
                  jint arg5,
                  jint arg6,
                  jobjectArray arg7,
                  jobjectArray arg8)
#else /* JAVA_SPEC_VERSION >= 22 */
JVM_MoreStackWalk(JNIEnv *env,
                  jobject arg1,
                  jlong arg2,
                  jlong arg3,
                  jint arg4,
                  jint arg5,
                  jobjectArray arg6,
                  jobjectArray arg7)
#endif /* JAVA_SPEC_VERSION >= 22 */
{
	assert(!"JVM_MoreStackWalk unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
	return -1;
}

jint JNICALL
JVM_ConstantPoolGetClassRefIndexAt(JNIEnv *env, jobject arg1, jlong arg2, jint arg3)
{
	assert(!"JVM_ConstantPoolGetClassRefIndexAt unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
	return -1;
}

jobjectArray JNICALL
JVM_GetVmArguments(JNIEnv *env)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *internalFunctions = vm->internalVMFunctions;
	jobjectArray result = nullptr;
	J9Class *vmClass = nullptr;

	internalFunctions->internalEnterVMFromJNI(currentThread);

	vmClass = J9VMCOMIBMOTIVMVM_OR_NULL(vm);

	if (nullptr != vmClass) {
		J9Method *method = internalFunctions->findJNIMethod(currentThread, vmClass,
		                                                    const_cast<char *>("getVMArgs"),
		                                                    const_cast<char *>("()[Ljava/lang/String;"));

		if (nullptr != method) {
			jmethodID mid = reinterpret_cast<jmethodID>(internalFunctions->getJNIMethodID(currentThread, method));

			if (nullptr != mid) {
				jclass vmJniClass =
				        static_cast<jclass>(internalFunctions->j9jni_createLocalRef(env, vmClass->classObject));

				if (nullptr != vmJniClass) {
					/* exit vm before calling jni method */
					internalFunctions->internalExitVMToJNI(currentThread);

					result = static_cast<jobjectArray>(env->CallStaticObjectMethod(vmJniClass, mid));

					internalFunctions->internalEnterVMFromJNI(currentThread);
					internalFunctions->j9jni_deleteLocalRef(env, static_cast<jobject>(vmJniClass));
					goto success;
				}
			}
		}
	}
	/* if code reaches here, something went wrong */
	internalFunctions->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGINTERNALERROR, nullptr);

success:
	internalFunctions->internalExitVMToJNI(currentThread);
	return result;
}

void JNICALL
JVM_FillStackFrames(JNIEnv *env, jclass arg1, jint arg2, jobjectArray arg3, jint arg4, jint arg5)
{
	assert(!"JVM_FillStackFrames unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
}

jclass JNICALL
JVM_FindClassFromCaller(JNIEnv *env, const char *arg1, jboolean arg2, jobject arg3, jclass arg4)
{
	assert(!"JVM_FindClassFromCaller unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
	return nullptr;
}

jobjectArray JNICALL
JVM_ConstantPoolGetNameAndTypeRefInfoAt(JNIEnv *env, jobject arg1, jobject arg2, jint arg3)
{
	assert(!"JVM_ConstantPoolGetNameAndTypeRefInfoAt unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
	return nullptr;
}

jbyte JNICALL
JVM_ConstantPoolGetTagAt(JNIEnv *env, jobject arg1, jobject arg2, jint arg3)
{
	assert(!"JVM_ConstantPoolGetTagAt unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
	return 0;
}

jobject JNICALL
#if JAVA_SPEC_VERSION >= 22
JVM_CallStackWalk(JNIEnv *env,
                  jobject arg1,
                  jint arg2,
                  jint arg3,
                  jint arg4,
                  jint arg5,
                  jobjectArray arg6,
                  jobjectArray arg7)
#else /* JAVA_SPEC_VERSION >= 22 */
JVM_CallStackWalk(JNIEnv *env,
                  jobject arg1,
                  jlong arg2,
                  jint arg3,
                  jint arg4,
                  jint arg5,
                  jobjectArray arg6,
                  jobjectArray arg7)
#endif /* JAVA_SPEC_VERSION >= 22 */
{
	assert(!"JVM_CallStackWalk unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
	return nullptr;
}

JNIEXPORT jobject JNICALL
JVM_GetAndClearReferencePendingList(JNIEnv *env)
{
	assert(!"JVM_GetAndClearReferencePendingList unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
	return nullptr;
}

JNIEXPORT jboolean JNICALL
JVM_HasReferencePendingList(JNIEnv *env)
{
	assert(!"JVM_HasReferencePendingList unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
	return JNI_FALSE;
}

JNIEXPORT void JNICALL
JVM_WaitForReferencePendingList(JNIEnv *env)
{
	assert(!"JVM_WaitForReferencePendingList unimplemented"); /* Jazz 108925: Revive J9JCL raw pConfig build */
	return;
}

/**
 * Adds an unnamed module to the bootLoader
 * JDK21+ saves it to J9JavaVM->unamedModuleForSystemLoader->moduleObject,
 * and delays bootclassloader.unnamedModule setting until java.base module is created.
 *
 * @param module module
 *
 * @throws IllegalArgumentException is thrown if
 * 1) Module is named
 * 2) Module is not j.l.r.Module or subclass of
 * 3) Module is not loaded by the bootLoader
 *
 * @throws NullPointerException if module is nullptr
 */
void JNICALL
JVM_SetBootLoaderUnnamedModule(JNIEnv *env, jobject module)
{
	J9VMThread *const currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;

	vmFuncs->internalEnterVMFromJNI(currentThread);
	if (nullptr == module) {
		vmFuncs->setCurrentExceptionUTF(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION,
		                                "module is null");
	} else {
		j9object_t modObj = J9_JNI_UNWRAP_REFERENCE(module);
		J9ClassLoader *systemClassLoader = vm->systemClassLoader;
		J9Class *instanceClazz = J9OBJECT_CLAZZ(currentThread, modObj);
		if (nullptr == currentThread->currentException) {
			J9Class *moduleClass = vmFuncs->internalFindKnownClass(
			        currentThread, J9VMCONSTANTPOOL_JAVALANGMODULE, J9_FINDKNOWNCLASS_FLAG_INITIALIZE);
			if (!isModuleUnnamed(currentThread, modObj)) {
				vmFuncs->setCurrentExceptionUTF(currentThread,
				                                J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
				                                "named module was supplied");
			} else if (!isSameOrSuperClassOf(moduleClass, instanceClazz)) {
				vmFuncs->setCurrentExceptionUTF(
				        currentThread, J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
				        "module supplied is not same or sub class of java/lang/Module");
			} else if (instanceClazz->classLoader != systemClassLoader) {
				vmFuncs->setCurrentExceptionUTF(currentThread,
				                                J9VMCONSTANTPOOL_JAVALANGILLEGALARGUMENTEXCEPTION,
				                                "module was not loaded by the bootclassloader");
			} else {
#if JAVA_SPEC_VERSION >= 21
				J9Module *unamedModuleForSystemLoader = vm->unamedModuleForSystemLoader;
				/* can't set bootclassloader.unnamedModule since bootclassloader hasn't finished the initialization yet */
				if (nullptr == unamedModuleForSystemLoader) {
					vmFuncs->setCurrentExceptionUTF(
					        currentThread, J9VMCONSTANTPOOL_JAVALANGINTERNALERROR,
					        "unamedModuleForSystemLoader was not initialized");
				} else if (nullptr != unamedModuleForSystemLoader->moduleObject) {
					vmFuncs->setCurrentExceptionUTF(
					        currentThread, J9VMCONSTANTPOOL_JAVALANGINTERNALERROR,
					        "module is already set in the unamedModuleForSystemLoader");
				} else {
					J9Module *j9mod = createModule(currentThread, modObj, systemClassLoader,
					                               nullptr /* nullptr name field */);
					unamedModuleForSystemLoader->moduleObject = modObj;
					Trc_MODULE_setUnamedModuleForSystemLoaderModuleObject(
					        currentThread, j9mod, unamedModuleForSystemLoader);
				}
#else /* JAVA_SPEC_VERSION >= 21 */
				if (nullptr
				    == J9VMJAVALANGCLASSLOADER_UNNAMEDMODULE(currentThread,
				                                             systemClassLoader->classLoaderObject)) {
					J9Module *j9mod = createModule(currentThread, modObj, systemClassLoader,
					                               nullptr /* nullptr name field */);
					J9VMJAVALANGCLASSLOADER_SET_UNNAMEDMODULE(
					        currentThread, systemClassLoader->classLoaderObject, modObj);
					Trc_MODULE_setBootloaderUnnamedModule(currentThread, j9mod);
				} else {
					vmFuncs->setCurrentExceptionUTF(currentThread,
					                                J9VMCONSTANTPOOL_JAVALANGINTERNALERROR,
					                                "module is already set in the bootclassloader");
				}
#endif /* JAVA_SPEC_VERSION >= 21 */
			}
		}
	}
	vmFuncs->internalExitVMToJNI(currentThread);
}

void JNICALL
JVM_ToStackTraceElement(JNIEnv *env, jobject arg1, jobject arg2)
{
	assert(!"JVM_ToStackTraceElement unimplemented");
}

void JNICALL
JVM_GetStackTraceElements(JNIEnv *env, jobject throwable, jobjectArray elements)
{
	assert(!"JVM_GetStackTraceElements unimplemented");
}

void JNICALL
JVM_InitStackTraceElementArray(JNIEnv *env, jobjectArray elements, jobject throwable)
{
	assert(!"JVM_InitStackTraceElementArray unimplemented");
}

void JNICALL
JVM_InitStackTraceElement(JNIEnv *env, jobject element, jobject stackFrameInfo)
{
	assert(!"JVM_InitStackTraceElement unimplemented");
}

/**
 * Return the clock time in nanoseconds at given offset
 *
 * @param [in] env pointer to JNIEnv
 * @param [in] clazz Class object
 * @param [in] offsetSeconds offset in seconds
 *
 * @return nanoSeconds, -1 on failure
 */
jlong JNICALL
JVM_GetNanoTimeAdjustment(JNIEnv *env, jclass clazz, jlong offsetSeconds)
{
	PORT_ACCESS_FROM_ENV(env);
	jlong offsetNanoSeconds = 0;
	jlong currentTimeNano = 0;
	jlong result = -1;

	/* 2^63/10^9 is the largest number offsetSeconds can be such that multiplying it
	 * by J9TIME_NANOSECONDS_PER_SECOND (10^9) will not result in an overflow
	 */
	if ((offsetSeconds <= OFFSET_MAX) && (offsetSeconds >= OFFSET_MIN)) {
		UDATA success = 0;
		offsetNanoSeconds = offsetSeconds * J9TIME_NANOSECONDS_PER_SECOND;
		currentTimeNano = static_cast<jlong>(j9time_current_time_nanos(&success));
		if (success) {
			if ((offsetNanoSeconds >= (currentTimeNano - TIME_LONG_MAX))
			    && (offsetNanoSeconds <= (currentTimeNano - TIME_LONG_MIN))) {
				result = currentTimeNano - offsetNanoSeconds;
			}
		}
	}

	return result;
}

JNIEXPORT jclass JNICALL
JVM_GetNestHost(JNIEnv *env, jclass clz)
{
	assert(!"JVM_GetNestHost unimplemented");
	return nullptr;
}

JNIEXPORT jobjectArray JNICALL
JVM_GetNestMembers(JNIEnv *env, jclass clz)
{
	assert(!"JVM_GetNestMembers unimplemented");
	return nullptr;
}

/**
 * Check if two classes belong to the same nest
 *
 * @param [in] env pointer to JNIEnv
 * @param [in] jClassOne Class object 1
 * @param [in] jClassTwo Class object 2
 *
 * @return JNI_TRUE if classes belong to the same nest, JNI_FALSE otherwise
 */
JNIEXPORT jboolean JNICALL
JVM_AreNestMates(JNIEnv *env, jclass jClassOne, jclass jClassTwo)
{
	jboolean result = JNI_FALSE;

	if ((nullptr != jClassOne) && (nullptr != jClassTwo)) {
		j9object_t clazzObjectOne = nullptr;
		j9object_t clazzObjectTwo = nullptr;
		J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
		J9InternalVMFunctions *vmFuncs = currentThread->javaVM->internalVMFunctions;

		vmFuncs->internalEnterVMFromJNI(currentThread);
		clazzObjectOne = J9_JNI_UNWRAP_REFERENCE(jClassOne);
		clazzObjectTwo = J9_JNI_UNWRAP_REFERENCE(jClassTwo);

		if (clazzObjectOne == clazzObjectTwo) {
			result = JNI_TRUE;
		} else {
			J9Class *clazzOne = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, clazzObjectOne);
			J9Class *clazzTwo = J9VM_J9CLASS_FROM_HEAPCLASS(currentThread, clazzObjectTwo);
			J9Class *clazzOneNestHost = clazzOne->nestHost;
			J9Class *clazzTwoNestHost = nullptr;

			if (nullptr == clazzOneNestHost) {
				if (J9_VISIBILITY_ALLOWED
				    != vmFuncs->loadAndVerifyNestHost(currentThread, clazzOne, J9_LOOK_NO_THROW,
				                                      &clazzOneNestHost)) {
					goto done;
				}
			}
			clazzTwoNestHost = clazzTwo->nestHost;
			if (nullptr == clazzTwoNestHost) {
				if (J9_VISIBILITY_ALLOWED
				    != vmFuncs->loadAndVerifyNestHost(currentThread, clazzTwo, J9_LOOK_NO_THROW,
				                                      &clazzTwoNestHost)) {
					goto done;
				}
			}

			if (clazzOneNestHost == clazzTwoNestHost) {
				result = JNI_TRUE;
			}
		}
	done:
		vmFuncs->internalExitVMToJNI(currentThread);
	}

	return result;
}
#endif /* JAVA_SPEC_VERSION >= 11 */

#if JAVA_SPEC_VERSION >= 15
JNIEXPORT void JNICALL
JVM_RegisterLambdaProxyClassForArchiving(JNIEnv *env,
                                         jclass arg1,
                                         jstring arg2,
                                         jobject arg3,
                                         jobject arg4,
                                         jobject arg5,
                                         jobject arg6,
                                         jclass arg7)
{
	assert(!"JVM_RegisterLambdaProxyClassForArchiving unimplemented");
}

JNIEXPORT jclass JNICALL
JVM_LookupLambdaProxyClassFromArchive(JNIEnv *env,
                                      jclass arg1,
                                      jstring arg2,
                                      jobject arg3,
                                      jobject arg4,
                                      jobject arg5,
                                      jobject arg6
#if JAVA_SPEC_VERSION == 15
                                      ,
                                      jboolean arg7
#endif /* JAVA_SPEC_VERSION == 15 */
)
{
	assert(!"JVM_LookupLambdaProxyClassFromArchive unimplemented");
	return nullptr;
}

#if JAVA_SPEC_VERSION < 23
JNIEXPORT jboolean JNICALL
JVM_IsCDSDumpingEnabled(JNIEnv *env)
{
	/* OpenJ9 does not support -Xshare:dump, so we return false unconditionally. */
	return JNI_FALSE;
}
#endif /* JAVA_SPEC_VERSION < 23 */
#endif /* JAVA_SPEC_VERSION >= 15 */

#if JAVA_SPEC_VERSION >= 16

JNIEXPORT jlong JNICALL
JVM_GetRandomSeedForDumping()
{
	/* OpenJ9 does not support -Xshare:dump, so we return zero unconditionally. */
	return 0;
}

#if JAVA_SPEC_VERSION < 23
JNIEXPORT jboolean JNICALL
JVM_IsDumpingClassList(JNIEnv *env)
{
	return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
JVM_IsSharingEnabled(JNIEnv *env)
{
	/* OpenJ9 does not support CDS, so we return false unconditionally. */
	return JNI_FALSE;
}
#endif /* JAVA_SPEC_VERSION < 23 */
#endif /* JAVA_SPEC_VERSION >= 16 */

JNIEXPORT jboolean JNICALL
JVM_IsUseContainerSupport(JNIEnv *env)
{
	J9VMThread *const currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	jboolean result = JNI_FALSE;

	if (J9_ARE_ALL_BITS_SET(vm->extendedRuntimeFlags2, J9_EXTENDED_RUNTIME2_USE_CONTAINER_SUPPORT)) {
		/* Return true if -XX:+UseContainerSupport is specified. This option is enabled by default. */
		result = JNI_TRUE;
	}

	return result;
}

// end of java11vmi.c

#if JAVA_SPEC_VERSION >= 19
extern J9JavaVM *BFUjavaVM;

extern IDATA (*f_threadSleep)(I_64 millis);
#endif /* JAVA_SPEC_VERSION >= 19 */

/* Define for debug
#define DEBUG_BCV
*/

#if JAVA_SPEC_VERSION >= 16
JNIEXPORT void JNICALL
JVM_DefineArchivedModules(JNIEnv *env, jobject obj1, jobject obj2)
{
	Assert_SC_true(!"JVM_DefineArchivedModules unimplemented");
}

JNIEXPORT void JNICALL
JVM_LogLambdaFormInvoker(JNIEnv *env, jstring str)
{
	Assert_SC_true(!"JVM_LogLambdaFormInvoker unimplemented");
}
#endif /* JAVA_SPEC_VERSION >= 16 */

#if JAVA_SPEC_VERSION >= 11
JNIEXPORT void JNICALL
JVM_InitializeFromArchive(JNIEnv *env, jclass clz)
{
	/* A no-op implementation is ok. */
}
#endif /* JAVA_SPEC_VERSION >= 11 */

#if JAVA_SPEC_VERSION >= 14
typedef struct GetNPEStackTraceElementUserData
{
	J9ROMClass *romClass;
	J9ROMMethod *romMethod;
	UDATA bytecodeOffset;
} GetNPEStackTraceElementUserData;

static UDATA
getNPEStackTraceElementIterator(J9VMThread *vmThread,
                                void *voidUserData,
                                UDATA bytecodeOffset,
                                J9ROMClass *romClass,
                                J9ROMMethod *romMethod,
                                J9UTF8 *fileName,
                                UDATA lineNumber,
                                J9ClassLoader *classLoader,
                                J9Class *ramClass)
{
	UDATA result = J9_STACKWALK_STOP_ITERATING;

	if ((nullptr != romMethod) && J9_ARE_ALL_BITS_SET(romMethod->modifiers, J9AccMethodFrameIteratorSkip)) {
		/* Skip methods with java.lang.invoke.FrameIteratorSkip / jdk.internal.vm.annotation.Hidden / java.lang.invoke.LambdaForm$Hidden annotation */
		result = J9_STACKWALK_KEEP_ITERATING;
	} else {
		GetNPEStackTraceElementUserData *userData = reinterpret_cast<GetNPEStackTraceElementUserData *>(voidUserData);

		/* We are done, first non-hidden stack frame is found. */
		userData->romClass = romClass;
		userData->romMethod = romMethod;
		userData->bytecodeOffset = bytecodeOffset;
	}
	return result;
}

#if defined(DEBUG_BCV)
static void
cfdumpBytecodePrintFunction(void *userData, char *format, ...)
{
	PORT_ACCESS_FROM_PORT(reinterpret_cast<J9PortLibrary *>(userData));
	va_list args;
	char outputBuffer[512] = {0};

	va_start(args, format);
	j9str_vprintf(outputBuffer, 512, format, args);
	va_end(args);
	j9tty_printf(PORTLIB, "%s", outputBuffer);
}
#endif /* defined(DEBUG_BCV) */

JNIEXPORT jstring JNICALL
JVM_GetExtendedNPEMessage(JNIEnv *env, jthrowable throwableObj)
{
	J9VMThread *vmThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = vmThread->javaVM;
	jobject msgObjectRef = nullptr;

	Trc_SC_GetExtendedNPEMessage_Entry(vmThread, throwableObj);
	if (J9_ARE_ANY_BITS_SET(vm->extendedRuntimeFlags2, J9_EXTENDED_RUNTIME2_SHOW_EXTENDED_NPEMSG)) {
		J9InternalVMFunctions const *const vmFuncs = vm->internalVMFunctions;
		char *npeMsg = nullptr;
		GetNPEStackTraceElementUserData userData = {0};
		/* If -XX:+ShowHiddenFrames option has not been set, skip hidden method frames */
		UDATA skipHiddenFrames = J9_ARE_NO_BITS_SET(vm->runtimeFlags, J9_RUNTIME_SHOW_HIDDEN_FRAMES);

		Trc_SC_GetExtendedNPEMessage_Entry2(vmThread, throwableObj);
		vmFuncs->internalEnterVMFromJNI(vmThread);
		userData.bytecodeOffset = UDATA_MAX;
		vmFuncs->iterateStackTrace(vmThread, reinterpret_cast<j9object_t *>(throwableObj), getNPEStackTraceElementIterator,
		                           &userData, TRUE, skipHiddenFrames);
		if ((nullptr != userData.romClass) && (nullptr != userData.romMethod)
		    && (UDATA_MAX != userData.bytecodeOffset)) {
			PORT_ACCESS_FROM_VMC(vmThread);
			J9NPEMessageData npeMsgData = {0};
#if defined(DEBUG_BCV)
			{
				U_8 *bytecodes = J9_BYTECODE_START_FROM_ROM_METHOD(userData.romMethod);
				U_32 flags = 0;

#if defined(J9VM_ENV_LITTLE_ENDIAN)
				flags |= BCT_LittleEndianOutput;
#else /* defined(J9VM_ENV_LITTLE_ENDIAN) */
				flags |= BCT_BigEndianOutput;
#endif /* defined(J9VM_ENV_LITTLE_ENDIAN) */
				j9bcutil_dumpBytecodes(PORTLIB, userData.romClass, bytecodes, 0,
				                       userData.bytecodeOffset, flags,
				                       cfdumpBytecodePrintFunction, PORTLIB, "");
			}
#endif /* defined(DEBUG_BCV) */
			npeMsgData.npePC = userData.bytecodeOffset;
			npeMsgData.vmThread = vmThread;
			npeMsgData.romClass = userData.romClass;
			npeMsgData.romMethod = userData.romMethod;
			npeMsg = vmFuncs->getNPEMessage(&npeMsgData);
			if (nullptr != npeMsg) {
				j9object_t msgObject = vm->memoryManagerFunctions->j9gc_createJavaLangString(
				        vmThread, reinterpret_cast<U_8 *>(npeMsg), strlen(npeMsg), 0);
				if (nullptr != msgObject) {
					msgObjectRef = vmFuncs->j9jni_createLocalRef(env, msgObject);
				}
				j9mem_free_memory(npeMsg);
			}
			j9mem_free_memory(npeMsgData.liveStack);
			j9mem_free_memory(npeMsgData.bytecodeOffset);
			j9mem_free_memory(npeMsgData.bytecodeMap);
			j9mem_free_memory(npeMsgData.stackMaps);
			j9mem_free_memory(npeMsgData.unwalkedQueue);
		} else {
			Trc_SC_GetExtendedNPEMessage_Null_NPE_MSG(vmThread, userData.romClass, userData.romMethod,
			                                          userData.bytecodeOffset);
		}
		vmFuncs->internalExitVMToJNI(vmThread);
	}
	Trc_SC_GetExtendedNPEMessage_Exit(vmThread, msgObjectRef);

	return static_cast<jstring>(msgObjectRef);
}
#endif /* JAVA_SPEC_VERSION >= 14 */

#if JAVA_SPEC_VERSION >= 17
JNIEXPORT void JNICALL
JVM_DumpClassListToFile(JNIEnv *env, jstring str)
{
	Assert_SC_true(!"JVM_DumpClassListToFile unimplemented");
}

JNIEXPORT void JNICALL
JVM_DumpDynamicArchive(JNIEnv *env, jstring str)
{
	Assert_SC_true(!"JVM_DumpDynamicArchive unimplemented");
}
#endif /* JAVA_SPEC_VERSION >= 17 */

#if JAVA_SPEC_VERSION >= 18
JNIEXPORT jboolean JNICALL
JVM_IsFinalizationEnabled(JNIEnv *env)
{
	jboolean isFinalizationEnabled = JNI_TRUE;
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	if (J9_ARE_ANY_BITS_SET(currentThread->javaVM->extendedRuntimeFlags2,
	                        J9_EXTENDED_RUNTIME2_DISABLE_FINALIZATION)) {
		isFinalizationEnabled = JNI_FALSE;
	}
	return isFinalizationEnabled;
}

JNIEXPORT void JNICALL
JVM_ReportFinalizationComplete(JNIEnv *env, jobject obj)
{
	Assert_SC_true(!"JVM_ReportFinalizationComplete unimplemented");
}
#endif /* JAVA_SPEC_VERSION >= 18 */

#if JAVA_SPEC_VERSION >= 19
JNIEXPORT void *JNICALL
JVM_LoadZipLibrary(void)
{
	void *zipHandle = nullptr;
	J9JavaVM *vm = BFUjavaVM;

	if (nullptr != vm) {
		PORT_ACCESS_FROM_JAVAVM(vm);
		uintptr_t handle = 0;

		if (J9PORT_SL_FOUND
		    == j9sl_open_shared_library(const_cast<char *>("zip"), &handle,
		                                OMRPORT_SLOPEN_DECORATE | OMRPORT_SLOPEN_LAZY)) {
			zipHandle = reinterpret_cast<void *>(handle);
		}
	}

	/* We may as well assert here: we won't make much progress without the library. */
	Assert_SC_notNull(zipHandle);

	return zipHandle;
}

JNIEXPORT void JNICALL
JVM_RegisterContinuationMethods(JNIEnv *env, jclass clz)
{
	Assert_SC_true(!"JVM_RegisterContinuationMethods unimplemented");
}

JNIEXPORT jboolean JNICALL
JVM_IsContinuationsSupported(void)
{
	return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
JVM_IsPreviewEnabled(void)
{
	jboolean isPreviewEnabled = JNI_FALSE;
	J9VMThread *currentThread = BFUjavaVM->internalVMFunctions->currentVMThread(BFUjavaVM);
	if (J9_ARE_ANY_BITS_SET(currentThread->javaVM->extendedRuntimeFlags2, J9_EXTENDED_RUNTIME2_ENABLE_PREVIEW)) {
		isPreviewEnabled = JNI_TRUE;
	}
	return isPreviewEnabled;
}

static void
enterVThreadTransitionCritical(J9VMThread *currentThread, jobject thread)
{
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;
	MM_ObjectAccessBarrierAPI objectAccessBarrier = MM_ObjectAccessBarrierAPI(currentThread);
	j9object_t threadObj = J9_JNI_UNWRAP_REFERENCE(thread);

retry:
	while (!objectAccessBarrier.inlineMixedObjectCompareAndSwapU64(
	        currentThread, threadObj, vm->virtualThreadInspectorCountOffset, 0, ~static_cast<U_64>(0))) {
		/* Thread is being inspected or unmounted, wait. */
		vmFuncs->internalReleaseVMAccess(currentThread);
		VM_AtomicSupport::yieldCPU();
		/* After wait, the thread may suspend here. */
		vmFuncs->internalAcquireVMAccess(currentThread);
		threadObj = J9_JNI_UNWRAP_REFERENCE(thread);
	}

	/* Link the current J9VMThread with the virtual thread object. */
	if (!objectAccessBarrier.inlineMixedObjectCompareAndSwapU64(
	            currentThread, threadObj, vm->internalSuspendStateOffset, J9_VIRTUALTHREAD_INTERNAL_STATE_NONE,
	            reinterpret_cast<U_64>(currentThread))) {
		/* If virtual thread is suspended while unmounted, reset the inspectorCount and do a wait and retry. */
		if (VM_VMHelpers::isThreadSuspended(currentThread, threadObj)) {
			J9OBJECT_I64_STORE(currentThread, threadObj, vm->virtualThreadInspectorCountOffset, 0);
		}
		vmFuncs->internalReleaseVMAccess(currentThread);
		/* Spin is used instead of the halt flag as we cannot guarantee suspend flag is still set now.
		 *
		 * TODO: Dynamically increase the sleep time to a bounded maximum.
		 */
		f_threadSleep(10);
		/* After wait, the thread may suspend here. */
		vmFuncs->internalAcquireVMAccess(currentThread);
		threadObj = J9_JNI_UNWRAP_REFERENCE(thread);
		goto retry;
	}
}

static void
exitVThreadTransitionCritical(J9VMThread *currentThread, jobject thread)
{
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;
	j9object_t vthread = J9_JNI_UNWRAP_REFERENCE(thread);
	MM_ObjectAccessBarrierAPI objectAccessBarrier = MM_ObjectAccessBarrierAPI(currentThread);

	/* Remove J9VMThread address from internalSuspendedState field, as the thread state is no longer in a transition. */
	while (!objectAccessBarrier.inlineMixedObjectCompareAndSwapU64(
	        currentThread, vthread, vm->internalSuspendStateOffset, reinterpret_cast<U_64>(currentThread),
	        J9_VIRTUALTHREAD_INTERNAL_STATE_NONE)) {
		/* Wait if the suspend flag is set. */
		vmFuncs->internalReleaseVMAccess(currentThread);
		VM_AtomicSupport::yieldCPU();
		/* After wait, the thread may suspend here. */
		vmFuncs->internalAcquireVMAccess(currentThread);
		vthread = J9_JNI_UNWRAP_REFERENCE(thread);
	}

	/* Update to virtualThreadInspectorCount must be after clearing isSuspendedInternal field to retain sync ordering. */
	Assert_SC_true(-1 == J9OBJECT_I64_LOAD(currentThread, vthread, vm->virtualThreadInspectorCountOffset));
	J9OBJECT_I64_STORE(currentThread, vthread, vm->virtualThreadInspectorCountOffset, 0);
}

static void
setContinuationStateToLastUnmount(J9VMThread *currentThread, jobject thread)
{
	enterVThreadTransitionCritical(currentThread, thread);
	/* Re-fetch reference as enterVThreadTransitionCritical may release VMAccess. */
	j9object_t threadObj = J9_JNI_UNWRAP_REFERENCE(thread);
	j9object_t continuationObj = J9VMJAVALANGVIRTUALTHREAD_CONT(currentThread, threadObj);
	ContinuationState volatile *continuationStatePtr =
	        VM_ContinuationHelpers::getContinuationStateAddress(currentThread, continuationObj);
	/* Used in JVMTI to not suspend the virtual thread once it enters the last unmount phase. */
	VM_ContinuationHelpers::setLastUnmount(continuationStatePtr);
	exitVThreadTransitionCritical(currentThread, thread);
}

/* Caller must have VMAccess. */
static void
virtualThreadMountBegin(JNIEnv *env, jobject thread)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);

	j9object_t threadObj = J9_JNI_UNWRAP_REFERENCE(thread);
	Assert_SC_true(IS_JAVA_LANG_VIRTUALTHREAD(currentThread, threadObj));

	if (TrcEnabled_Trc_SC_VirtualThread_Info) {
		J9JavaVM *vm = currentThread->javaVM;
		j9object_t continuationObj = J9VMJAVALANGVIRTUALTHREAD_CONT(currentThread, threadObj);
		J9VMContinuation *continuation = J9VMJDKINTERNALVMCONTINUATION_VMREF(currentThread, continuationObj);
		Trc_SC_VirtualThread_Info(
		        currentThread, threadObj, J9VMJAVALANGVIRTUALTHREAD_STATE(currentThread, threadObj),
		        J9OBJECT_I64_LOAD(currentThread, threadObj, vm->virtualThreadInspectorCountOffset),
		        J9VMJAVALANGVIRTUALTHREAD_CARRIERTHREAD(currentThread, threadObj), continuationObj,
		        continuation);
	}

	enterVThreadTransitionCritical(currentThread, thread);

	VM_VMHelpers::virtualThreadHideFrames(currentThread, JNI_TRUE);
}

/* Caller must have VMAccess. */
static void
virtualThreadMountEnd(JNIEnv *env, jobject thread)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	j9object_t threadObj = J9_JNI_UNWRAP_REFERENCE(thread);

	Assert_SC_true(IS_JAVA_LANG_VIRTUALTHREAD(currentThread, threadObj));

	if (TrcEnabled_Trc_SC_VirtualThread_Info) {
		j9object_t continuationObj = J9VMJAVALANGVIRTUALTHREAD_CONT(currentThread, threadObj);
		Trc_SC_VirtualThread_Info(
		        currentThread, threadObj, J9VMJAVALANGVIRTUALTHREAD_STATE(currentThread, threadObj),
		        J9OBJECT_I64_LOAD(currentThread, threadObj, vm->virtualThreadInspectorCountOffset),
		        J9VMJAVALANGVIRTUALTHREAD_CARRIERTHREAD(currentThread, threadObj), continuationObj,
		        J9VMJDKINTERNALVMCONTINUATION_VMREF(currentThread, continuationObj));
	}

	VM_VMHelpers::virtualThreadHideFrames(currentThread, JNI_FALSE);

	/* Allow thread to be inspected again. */
	exitVThreadTransitionCritical(currentThread, thread);

	TRIGGER_J9HOOK_VM_VIRTUAL_THREAD_MOUNT(vm->hookInterface, currentThread);
}

/* Caller must have VMAccess. */
static void
virtualThreadUnmountBegin(JNIEnv *env, jobject thread)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;

	j9object_t threadObj = J9_JNI_UNWRAP_REFERENCE(thread);

	Assert_SC_true(IS_JAVA_LANG_VIRTUALTHREAD(currentThread, threadObj));

	if (TrcEnabled_Trc_SC_VirtualThread_Info) {
		j9object_t continuationObj = J9VMJAVALANGVIRTUALTHREAD_CONT(currentThread, threadObj);
		Trc_SC_VirtualThread_Info(
		        currentThread, threadObj, J9VMJAVALANGVIRTUALTHREAD_STATE(currentThread, threadObj),
		        J9OBJECT_I64_LOAD(currentThread, threadObj, vm->virtualThreadInspectorCountOffset),
		        J9VMJAVALANGVIRTUALTHREAD_CARRIERTHREAD(currentThread, threadObj), continuationObj,
		        J9VMJDKINTERNALVMCONTINUATION_VMREF(currentThread, continuationObj));
	}

	TRIGGER_J9HOOK_VM_VIRTUAL_THREAD_UNMOUNT(vm->hookInterface, currentThread);

	enterVThreadTransitionCritical(currentThread, thread);

	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;
	j9object_t carrierThreadObject = currentThread->carrierThreadObject;
	/* Virtual thread is being umounted. If its carrier thread is suspended, spin until
	 * the carrier thread is resumed. The carrier thread should not be mounted until it
	 * is resumed.
	 */
	while (VM_VMHelpers::isThreadSuspended(currentThread, carrierThreadObject)) {
		exitVThreadTransitionCritical(currentThread, thread);
		vmFuncs->internalReleaseVMAccess(currentThread);
		/* Spin is used instead of the halt flag; otherwise, the virtual thread will
		 * show as suspended.
		 *
		 * TODO: Dynamically increase the sleep time to a bounded maximum.
		 */
		f_threadSleep(10);
		vmFuncs->internalAcquireVMAccess(currentThread);
		enterVThreadTransitionCritical(currentThread, thread);
		carrierThreadObject = currentThread->carrierThreadObject;
	}

	VM_VMHelpers::virtualThreadHideFrames(currentThread, JNI_TRUE);
}

/* Caller must have VMAccess. */
static void
virtualThreadUnmountEnd(JNIEnv *env, jobject thread)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;

	j9object_t threadObj = J9_JNI_UNWRAP_REFERENCE(thread);
	j9object_t continuationObj = J9VMJAVALANGVIRTUALTHREAD_CONT(currentThread, threadObj);
	ContinuationState continuationState =
	        *VM_ContinuationHelpers::getContinuationStateAddress(currentThread, continuationObj);

	Assert_SC_true(IS_JAVA_LANG_VIRTUALTHREAD(currentThread, threadObj));

	if (TrcEnabled_Trc_SC_VirtualThread_Info) {
		Trc_SC_VirtualThread_Info(
		        currentThread, threadObj, J9VMJAVALANGVIRTUALTHREAD_STATE(currentThread, threadObj),
		        J9OBJECT_I64_LOAD(currentThread, threadObj, vm->virtualThreadInspectorCountOffset),
		        J9VMJAVALANGVIRTUALTHREAD_CARRIERTHREAD(currentThread, threadObj), continuationObj,
		        J9VMJDKINTERNALVMCONTINUATION_VMREF(currentThread, continuationObj));
	}

	if (VM_ContinuationHelpers::isFinished(continuationState)) {
		vmFuncs->freeTLS(currentThread, threadObj);
	}

	VM_VMHelpers::virtualThreadHideFrames(currentThread, JNI_FALSE);

	/* Allow thread to be inspected again. */
	exitVThreadTransitionCritical(currentThread, thread);
}
#endif /* JAVA_SPEC_VERSION >= 19 */

#if (19 <= JAVA_SPEC_VERSION) && (JAVA_SPEC_VERSION < 21)
JNIEXPORT void JNICALL
JVM_VirtualThreadMountBegin(JNIEnv *env, jobject thread, jboolean firstMount)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;

	Trc_SC_VirtualThreadMountBegin_Entry(currentThread, thread, firstMount);

	vmFuncs->internalEnterVMFromJNI(currentThread);

	virtualThreadMountBegin(env, thread);

	vmFuncs->internalExitVMToJNI(currentThread);

	Trc_SC_VirtualThreadMountBegin_Exit(currentThread, thread, firstMount);
}

JNIEXPORT void JNICALL
JVM_VirtualThreadMountEnd(JNIEnv *env, jobject thread, jboolean firstMount)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;

	Trc_SC_VirtualThreadMountEnd_Entry(currentThread, thread, firstMount);

	vmFuncs->internalEnterVMFromJNI(currentThread);

	virtualThreadMountEnd(env, thread);
	if (firstMount) {
		TRIGGER_J9HOOK_VM_VIRTUAL_THREAD_STARTED(vm->hookInterface, currentThread);
	}

	vmFuncs->internalExitVMToJNI(currentThread);

	Trc_SC_VirtualThreadMountEnd_Exit(currentThread, thread, firstMount);
}

JNIEXPORT void JNICALL
JVM_VirtualThreadUnmountBegin(JNIEnv *env, jobject thread, jboolean lastUnmount)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;

	Trc_SC_VirtualThreadUnmountBegin_Entry(currentThread, thread, lastUnmount);

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if (lastUnmount) {
		TRIGGER_J9HOOK_VM_VIRTUAL_THREAD_END(vm->hookInterface, currentThread);
		setContinuationStateToLastUnmount(reinterpret_cast<J9VMThread *>(env), thread);
	}
	virtualThreadUnmountBegin(env, thread);

	vmFuncs->internalExitVMToJNI(currentThread);

	Trc_SC_VirtualThreadUnmountBegin_Exit(currentThread, thread, lastUnmount);
}

JNIEXPORT void JNICALL
JVM_VirtualThreadUnmountEnd(JNIEnv *env, jobject thread, jboolean lastUnmount)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;

	Trc_SC_VirtualThreadUnmountEnd_Entry(currentThread, thread, lastUnmount);

	vmFuncs->internalEnterVMFromJNI(currentThread);

	virtualThreadUnmountEnd(env, thread);

	vmFuncs->internalExitVMToJNI(currentThread);

	Trc_SC_VirtualThreadUnmountEnd_Exit(currentThread, thread, lastUnmount);
}
#endif /* (19 <= JAVA_SPEC_VERSION) && (JAVA_SPEC_VERSION < 21) */

#if JAVA_SPEC_VERSION >= 20
JNIEXPORT jint JNICALL
JVM_GetClassFileVersion(JNIEnv *env, jclass cls)
{
	jint version = 0;
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9InternalVMFunctions const *const vmFuncs = currentThread->javaVM->internalVMFunctions;

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if (nullptr == cls) {
		vmFuncs->setCurrentException(currentThread, J9VMCONSTANTPOOL_JAVALANGNULLPOINTEREXCEPTION, nullptr);
	} else {
		J9Class *clazz = J9VM_J9CLASS_FROM_JCLASS(currentThread, cls);
		version = static_cast<jint>(getClassFileVersion(currentThread, clazz));
	}

	vmFuncs->internalExitVMToJNI(currentThread);

	return version;
}

JNIEXPORT void JNICALL
JVM_VirtualThreadHideFrames(JNIEnv *env,
#if JAVA_SPEC_VERSION >= 23
                            jclass clz,
#else /* JAVA_SPEC_VERSION >= 23 */
                            jobject vthread,
#endif /* JAVA_SPEC_VERSION >= 23 */
                            jboolean hide)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9InternalVMFunctions const *const vmFuncs = currentThread->javaVM->internalVMFunctions;

	vmFuncs->internalEnterVMFromJNI(currentThread);

	j9object_t vThreadObj = currentThread->threadObject;
	Assert_SC_true(IS_JAVA_LANG_VIRTUALTHREAD(currentThread, vThreadObj));
	/* Do not allow JVMTI operations because J9VMThread->threadObject is modified
	 * between the first invocation with hide=true and the second invocation with
	 * hide=false. Otherwise, JVMTI functions will see an unstable
	 * J9VMThread->threadObject.
	 */
	bool hiddenFrames =
	        J9_ARE_ALL_BITS_SET(currentThread->privateFlags, J9_PRIVATE_FLAGS_VIRTUAL_THREAD_HIDDEN_FRAMES);
	if (hide) {
		Assert_SC_true(!hiddenFrames);
#if JAVA_SPEC_VERSION < 23
		Assert_SC_true(vThreadObj == J9_JNI_UNWRAP_REFERENCE(vthread));
#endif /* JAVA_SPEC_VERSION < 23 */
		enterVThreadTransitionCritical(currentThread, (jobject)&currentThread->threadObject);
	}

	VM_VMHelpers::virtualThreadHideFrames(currentThread, hide);

	if (!hide) {
		Assert_SC_true(hiddenFrames);
		exitVThreadTransitionCritical(currentThread, (jobject)&currentThread->threadObject);
	}

	vmFuncs->internalExitVMToJNI(currentThread);
}
#endif /* JAVA_SPEC_VERSION >= 20 */

#if JAVA_SPEC_VERSION >= 21
JNIEXPORT jboolean JNICALL
JVM_PrintWarningAtDynamicAgentLoad()
{
	jboolean result = JNI_TRUE;
	J9JavaVM *vm = BFUjavaVM;
	if (J9_ARE_ANY_BITS_SET(vm->runtimeFlags, J9_RUNTIME_ALLOW_DYNAMIC_AGENT)
	    && (0 <= FIND_ARG_IN_VMARGS(EXACT_MATCH, VMOPT_XXENABLEDYNAMICAGENTLOADING, nullptr))) {
		result = JNI_FALSE;
	}
	return result;
}

JNIEXPORT void JNICALL
JVM_VirtualThreadMount(JNIEnv *env, jobject vthread, jboolean hide)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;

	Trc_SC_VirtualThreadMount_Entry(currentThread, vthread, hide);

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if (hide) {
		virtualThreadMountBegin(env, vthread);
	} else {
		virtualThreadMountEnd(env, vthread);
	}

	vmFuncs->internalExitVMToJNI(currentThread);

	Trc_SC_VirtualThreadMount_Exit(currentThread, vthread, hide);
}

JNIEXPORT void JNICALL
JVM_VirtualThreadUnmount(JNIEnv *env, jobject vthread, jboolean hide)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;

	Trc_SC_VirtualThreadUnmount_Entry(currentThread, vthread, hide);

	vmFuncs->internalEnterVMFromJNI(currentThread);

	if (hide) {
		virtualThreadUnmountBegin(env, vthread);
	} else {
		virtualThreadUnmountEnd(env, vthread);
	}

	vmFuncs->internalExitVMToJNI(currentThread);

	Trc_SC_VirtualThreadUnmount_Exit(currentThread, vthread, hide);
}

JNIEXPORT jboolean JNICALL
JVM_IsForeignLinkerSupported()
{
	return JNI_TRUE;
}

JNIEXPORT void JNICALL
JVM_VirtualThreadStart(JNIEnv *env, jobject vthread)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;

	Trc_SC_VirtualThreadStart_Entry(currentThread, vthread);

	vmFuncs->internalEnterVMFromJNI(currentThread);

	virtualThreadMountEnd(env, vthread);
	TRIGGER_J9HOOK_VM_VIRTUAL_THREAD_STARTED(vm->hookInterface, currentThread);

	vmFuncs->internalExitVMToJNI(currentThread);

	Trc_SC_VirtualThreadStart_Exit(currentThread, vthread);
}

JNIEXPORT void JNICALL
JVM_VirtualThreadEnd(JNIEnv *env, jobject vthread)
{
	J9VMThread *currentThread = reinterpret_cast<J9VMThread *>(env);
	J9JavaVM *vm = currentThread->javaVM;
	J9InternalVMFunctions *vmFuncs = vm->internalVMFunctions;

	Trc_SC_VirtualThreadEnd_Entry(currentThread, vthread);

	vmFuncs->internalEnterVMFromJNI(currentThread);

	TRIGGER_J9HOOK_VM_VIRTUAL_THREAD_END(vm->hookInterface, currentThread);
	setContinuationStateToLastUnmount(currentThread, vthread);
	virtualThreadUnmountBegin(env, vthread);

	vmFuncs->internalExitVMToJNI(currentThread);

	Trc_SC_VirtualThreadEnd_Exit(currentThread, vthread);
}
#endif /* JAVA_SPEC_VERSION >= 21 */

#if defined(J9VM_OPT_VALHALLA_VALUE_TYPES)
JNIEXPORT jboolean JNICALL
JVM_IsValhallaEnabled()
{
	return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
JVM_IsImplicitlyConstructibleClass(JNIEnv *env, jclass cls)
{
	assert(!"JVM_IsImplicitlyConstructibleClass unimplemented");
	return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
JVM_IsNullRestrictedArray(JNIEnv *env, jobject obj)
{
	assert(!"JVM_IsNullRestrictedArray unimplemented");
	return JNI_FALSE;
}

JNIEXPORT jarray JNICALL
JVM_NewNullRestrictedArray(JNIEnv *env, jclass cls, jint length)
{
	assert(!"JVM_NewNullRestrictedArray unimplemented");
	return nullptr;
}
#endif /* defined(J9VM_OPT_VALHALLA_VALUE_TYPES) */

#if JAVA_SPEC_VERSION >= 22
JNIEXPORT void JNICALL
JVM_ExpandStackFrameInfo(JNIEnv *env, jobject object)
{
	assert(!"JVM_ExpandStackFrameInfo unimplemented");
}

JNIEXPORT void JNICALL
JVM_VirtualThreadDisableSuspend(JNIEnv *env,
#if JAVA_SPEC_VERSION >= 23
                                jclass clz,
#else /* JAVA_SPEC_VERSION >= 23 */
                                jobject vthread,
#endif /* JAVA_SPEC_VERSION >= 23 */
                                jboolean enter)
{
	/* TODO: Add implementation.
	 * See https://github.com/eclipse-openj9/openj9/issues/18671 for more details.
	 */
}
#endif /* JAVA_SPEC_VERSION >= 22 */

#if JAVA_SPEC_VERSION >= 23
JNIEXPORT jint JNICALL
JVM_GetCDSConfigStatus()
{
	/* OpenJ9 does not support CDS, so we return 0 to indicate that there is no CDS config available. */
	return 0;
}
#endif /* JAVA_SPEC_VERSION >= 23 */

// end of javanextvmi.cpp

} /* extern "C" */
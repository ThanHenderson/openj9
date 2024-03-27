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
#include "ResolvedMethodNameTable.hpp"

#include "hashtable_api.h"
#include "j2sever.h"
#include "j9consts.h"
#include "j9protos.h"
#include "objhelp.h"
#include "ModronAssertions.h"

#include "EnvironmentBase.hpp"
#include "GCExtensions.hpp"
#include "VMHelpers.hpp"

extern "C" {

typedef struct ResolvedMethodNameEntry {
	j9object_t resolvedMethodName; /**< Interned ResolvedMethodName containing vmtarget and vmholder */
	UDATA hash; /** Corresponding hash value based on class name and method name and signature */
} ResolvedMethodNameEntry;

static UDATA resolvedMethodNameHashFn(void* key, void* userData);
static UDATA resolvedMethodNameHashEqualFn(void* leftKey, void* rightKey, void* userData);

MM_ResolvedMethodNameTable*
MM_ResolvedMethodNameTable::newInstance(MM_EnvironmentBase* env)
{
	MM_ResolvedMethodNameTable* table = static_cast<MM_ResolvedMethodNameTable*>(env->getForge()->allocate(
	        sizeof(MM_ResolvedMethodNameTable), MM_AllocationCategory::FIXED, J9_GET_CALLSITE()));
	if (NULL != table) {
		new (table) MM_ResolvedMethodNameTable(env);
		if (!table->initialize(env)) {
			table->kill(env);
			table = NULL;
		}
	}

	return table;
}

bool
MM_ResolvedMethodNameTable::initialize(MM_EnvironmentBase* env)
{
	bool result = true;
	J9JavaVM* javaVM = (J9JavaVM*)env->getOmrVM()->_language_vm;
	U_32 initialSize = 128;

	_table = hashTableNew(
	        OMRPORT_FROM_J9PORT(javaVM->portLibrary), J9_GET_CALLSITE(), initialSize, sizeof(ResolvedMethodNameEntry),
	        sizeof(void*), 0, OMRMEM_CATEGORY_MM, resolvedMethodNameHashFn, resolvedMethodNameHashEqualFn, NULL,
	        javaVM);
	if (NULL == _table) {
		result = false;
	}

	if (0 != omrthread_monitor_init_with_name(&_mutex, 0, "GC resolved MemberName table")) {
		result = false;
	}

	return result;
}

void
MM_ResolvedMethodNameTable::tearDown(MM_EnvironmentBase* env)
{
	if (NULL != _table) {
		hashTableFree(_table);
		_table = NULL;
	}

	if (NULL != _mutex) {
		omrthread_monitor_destroy(_mutex);
		_mutex = NULL;
	}
}

void
MM_ResolvedMethodNameTable::kill(MM_EnvironmentBase* env)
{
	tearDown(env);
	env->getForge()->free(this);
}

void
MM_ResolvedMethodNameTable::lockTable()
{
	omrthread_monitor_enter(_mutex);
}

void
MM_ResolvedMethodNameTable::unlockTable()
{
	omrthread_monitor_exit(_mutex);
}

UDATA
MM_ResolvedMethodNameTable::calculateHash(const J9UTF8* className, const J9NameAndSignature* nameAndSig)
{
	U_8* classNameData = J9UTF8_DATA(className);
	UDATA classNameLength = static_cast<UDATA>(J9UTF8_LENGTH(className));
	UDATA classNameHash = VM_VMHelpers::computeHashForUTF8(classNameData, classNameLength);

	J9UTF8* nameUTF = nameAndSig->name;
	U_8* name = J9UTF8_DATA(nameUTF);
	UDATA nameLength = static_cast<UDATA>(J9UTF8_LENGTH(nameUTF));
	UDATA nameHash = VM_VMHelpers::computeHashForUTF8(name, nameLength);

	J9UTF8* signature = nameAndSig->signature;
	U_8* signatureData = J9UTF8_DATA(signature);
	UDATA signatureLength = static_cast<UDATA>(J9UTF8_LENGTH(signature));
	UDATA signatureHash = VM_VMHelpers::computeHashForUTF8(signatureData, signatureLength);

	UDATA hash = classNameHash;
	hash = (hash * 31) ^ nameHash;
	hash = (hash * 31) ^ signatureHash;

	return hash;
}

j9object_t
MM_ResolvedMethodNameTable::find(J9VMThread* vmThread, const J9UTF8* className, const J9NameAndSignature* nameAndSig)
{
	UDATA hash = calculateHash(className, nameAndSig);

	return findInternal(vmThread, hash);
}

j9object_t
MM_ResolvedMethodNameTable::findInternal(J9VMThread* vmThread, UDATA hash)
{
	j9object_t result = NULL;
	ResolvedMethodNameEntry query = { NULL, hash };

	lockTable();
	ResolvedMethodNameEntry* entry =
	        static_cast<ResolvedMethodNameEntry*>(hashTableFind(_table, static_cast<void*>(&query)));
	unlockTable();

	if (NULL != entry) {
		// TODO: Need something here to check if entry is still live, and if not remove old entry from table?
		// 		 Something like the following?
		// if (isResolvedMethodNameEntryLive(vmThread->javaVM, entry)) {
		// 	result = entry->resolvedMethodName;
		// } else {
		// 	hashTableRemove(_table, entry);
		// 	result = NULL;
		// }
	J9JavaVM* vm = vmThread->javaVM;
		fprintf(stderr, "in find Hash: %ld, Method Hash: %ld\n", hash, entry->hash);
		void* method = (void*)J9OBJECT_U64_LOAD(vmThread, entry->resolvedMethodName, vm->vmtargetOffsetForResolvedMethodName);
		fprintf(stderr, "in find Method: %p\n", method);
		fprintf(stderr, "in find DeclaringClass: %p\n", (void*)J9OBJECT_U64_LOAD(vmThread, entry->resolvedMethodName, J9VMJAVALANGINVOKERESOLVEDMETHODNAME_VMHOLDER_OFFSET(vm)));
		result = entry->resolvedMethodName;
	}

	return result;
}

j9object_t
MM_ResolvedMethodNameTable::intern(
        J9VMThread* vmThread,
        const J9UTF8* className,
        const J9NameAndSignature* nameAndSig,
        j9object_t resolvedMethodName)
{
	UDATA hash = calculateHash(className, nameAndSig);
	j9object_t result = findInternal(vmThread, hash);
	if (NULL == result) {
		ResolvedMethodNameEntry query = { resolvedMethodName, hash };

		lockTable();
		ResolvedMethodNameEntry* entry =
		        static_cast<ResolvedMethodNameEntry*>(hashTableAdd(_table, static_cast<void*>(&query)));
		unlockTable();

		if (NULL != entry) {
		fprintf(stderr, "Intern Hash: %ld, Method: %p, Method Hash: %ld\n", hash, entry->resolvedMethodName, entry->hash);
			result = entry->resolvedMethodName;
		}
	}

	return result;
}

static UDATA
resolvedMethodNameHashFn(void* key, void* userData)
{
	auto query = static_cast<ResolvedMethodNameEntry*>(key);

	return query->hash;
}

static UDATA
resolvedMethodNameHashEqualFn(void* leftKey, void* rightKey, void* userData)
{
	auto left = static_cast<ResolvedMethodNameEntry*>(leftKey);
	auto right = static_cast<ResolvedMethodNameEntry*>(rightKey);

	return left->hash == right->hash;
}

j9object_t
j9gc_internResolvedMethodName(
        J9VMThread* vmThread,
        const J9UTF8* className,
        const J9NameAndSignature* nameAndSig,
        j9object_t method)
{
	J9JavaVM* vm = vmThread->javaVM;
	J9InternalVMFunctions* const vmFuncs = vm->internalVMFunctions;
	MM_GCExtensions* extensions = MM_GCExtensions::getExtensions(vm->omrVM);
	MM_ResolvedMethodNameTable* resolvedMethodNameTable = extensions->getResolvedMethodNameTable();

	j9object_t resolvedMethodName = resolvedMethodNameTable->find(vmThread, className, nameAndSig);
	if (NULL == resolvedMethodName) {	
		PUSH_OBJECT_IN_SPECIAL_FRAME(vmThread, method);
		resolvedMethodName = J9AllocateObject(
		        vmThread, J9VMJAVALANGINVOKERESOLVEDMETHODNAME(vm),
		        /* J9_GC_ALLOCATE_OBJECT_TENURED | */ J9_GC_ALLOCATE_OBJECT_NON_INSTRUMENTABLE);
		method = POP_OBJECT_IN_SPECIAL_FRAME(vmThread);
		if (NULL == resolvedMethodName) {
			vmFuncs->setHeapOutOfMemoryError(vmThread);
		}
		// set vm target with method
		// set vm holder with method.declaringClass()
		// fprintf(stderr, "interning method: %p\n", method);
		// fprintf(stderr, "with declaring class: %p\n", J9VMJAVALANGREFLECTMETHOD_CLAZZ(vmThread, method));
		J9OBJECT_U64_STORE(vmThread, resolvedMethodName, vm->vmtargetOffsetForResolvedMethodName, (U_64)method);
		J9OBJECT_U64_STORE(vmThread, resolvedMethodName, J9VMJAVALANGINVOKERESOLVEDMETHODNAME_VMHOLDER_OFFSET(vm), (U_64)J9VMJAVALANGREFLECTMETHOD_CLAZZ(vmThread, method));

		resolvedMethodName = resolvedMethodNameTable->intern(vmThread, className, nameAndSig, resolvedMethodName);
	}

	return resolvedMethodName;
}

j9object_t
j9gc_findResolvedMethodName(J9VMThread* vmThread, const J9UTF8* className, const J9NameAndSignature* nameAndSig)
{
	J9JavaVM* vm = vmThread->javaVM;
	MM_GCExtensions* extensions = MM_GCExtensions::getExtensions(vm->omrVM);
	MM_ResolvedMethodNameTable* resolvedMethodNameTable = extensions->getResolvedMethodNameTable();

	return resolvedMethodNameTable->find(vmThread, className, nameAndSig);
}
} /* end of extern "C" */

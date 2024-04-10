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
#include "MemberNameCache.hpp"

#include "hashtable_api.h"
#include "j9consts.h"
#include "j9protos.h"

#include "VMHelpers.hpp"

extern "C" {

typedef struct MemberNameEntry {
	jobject memberName; /**< Interned MemberName */
	UDATA hash; /** Corresponding hash value based on method name and signature */
} MemberNameEntry;

static UDATA memberNameHashFn(void *key, void *userData);
static UDATA memberNameHashEqualFn(void *leftKey, void *rightKey, void *userData);

static MemberNameCache *
MemberNameCache::newInstance(J9VMThread *vmThread)
{
	MemberNameCache *table =
	        static_cast<MemberNameCache *>(j9mem_allocate_memory(sizeof(MemberNameCache), OMRMEM_CATEGORY_VM));
	if (NULL != table) {
		new (table) MemberNameCache();
		if (!table->initialize(vmThread)) {
			table->kill(vmThread);
			table = NULL;
		}
	}

	return table;
}

bool
MemberNameCache::initialize(J9VMThread *vmThread)
{
	bool result = true;
	J9JavaVM *vm = vmThread->javaVM;
	U_32 initialSize = 128;

	_table = hashTableNew(
	        OMRPORT_FROM_J9PORT(PORTLIB), J9_GET_CALLSITE(), initialSize, sizeof(MemberNameEntry), sizeof(void *), 0,
	        OMRMEM_CATEGORY_VM, memberNameHashFn, memberNameHashEqualFn, NULL, vm);
	if (NULL == _table) {
		result = false;
	}

	if (0 != omrthread_monitor_init_with_name(&_mutex, 0, "Class-associated MemberName Cache")) {
		result = false;
	}

	return result;
}

void
MemberNameCache::kill(J9VMThread *vmThread)
{
	J9JavaVM *vm = vmThread->javaVM;
	J9InternalVMFunctions *const vmFuncs = vm->internalVMFunctions;
	for (i = 0; i < _table->tableSize; i++) {
		void *node = table->nodes[i];
		if (NULL != node) {
			MemberNameEntry *entry = static_cast<MemberNameEntry *>(node);
			vmFuncs->j9jni_deleteGlobalRef((JNIEnv *)vmThread, entry->memberName, JNI_TRUE);
		}
	}
	if (NULL != _table) {
		hashTableFree(_table);
		_table = NULL;
	}
	if (NULL != _mutex) {
		omrthread_monitor_destroy(_mutex);
		_mutex = NULL;
	}
	j9mem_free_memory(this);
}

void
MemberNameCache::lockTable()
{
	omrthread_monitor_enter(_mutex);
}

void
MemberNameCache::unlockTable()
{
	omrthread_monitor_exit(_mutex);
}

UDATA
MemberNameCache::calculateHash(const J9NameAndSignature *nameAndSig)
{
	J9UTF8 *nameUTF = nameAndSig->name;
	U_8 *name = J9UTF8_DATA(nameUTF);
	UDATA nameLength = static_cast<UDATA>(J9UTF8_LENGTH(nameUTF));
	UDATA nameHash = VM_VMHelpers::computeHashForUTF8(name, nameLength);

	J9UTF8 *signature = nameAndSig->signature;
	U_8 *signatureData = J9UTF8_DATA(signature);
	UDATA signatureLength = static_cast<UDATA>(J9UTF8_LENGTH(signature));
	UDATA signatureHash = VM_VMHelpers::computeHashForUTF8(signatureData, signatureLength);

	UDATA hash = nameHash;
	hash = (hash * 31) ^ signatureHash;

	return hash;
}

jobject
MemberNameCache::find(J9VMThread *vmThread, const J9NameAndSignature *nameAndSig)
{
	UDATA hash = calculateHash(nameAndSig);

	return findInternal(vmThread, hash);
}

jobject
MemberNameCache::findInternal(J9VMThread *vmThread, UDATA hash)
{
	J9JavaVM *vm = vmThread->javaVM;
	J9InternalVMFunctions *const vmFuncs = vm->internalVMFunctions;
	jobject result = NULL;
	MemberNameEntry query = { NULL, hash };

	lockTable();
	MemberNameEntry *entry = static_cast<MemberNameEntry *>(hashTableFind(_table, static_cast<void *>(&query)));
	unlockTable();

	if (NULL != entry) {
		j9object_t obj = J9_JNI_UNWRAP_REFERENCE(entry->memberName);
		if (NULL == obj) {
			vmFuncs->j9jni_deleteGlobalRef(reinterpret_cast<JNIEnv *>(vmThread), entry->memberName, JNI_TRUE);

			lockTable();
			hashTableRemove(_table, static_cast<void *>(entry));
			unlockTable();
		} else {
			result = entry->memberName;
		}
	}

	return result;
}

jobject
MemberNameCache::intern(J9VMThread *vmThread, const J9NameAndSignature *nameAndSig, jobject memberName)
{
	J9JavaVM *vm = vmThread->javaVM;
	J9InternalVMFunctions *const vmFuncs = vm->internalVMFunctions;
	UDATA hash = calculateHash(nameAndSig);
	jobject result = findInternal(vmThread, hash);
	if (NULL == result) {
		jobject weakRef = vmFuncs->j9jni_createGlobalRef((JNIEnv *)vmThread, memberName, JNI_TRUE);
		if (NULL == weakRef) {
			vmFuncs->setHeapOutOfMemoryError(vmThread);
			return nullptr;
		}
		MemberNameEntry query = { weakRef, hash };

		lockTable();
		MemberNameEntry *entry = static_cast<MemberNameEntry *>(hashTableAdd(_table, static_cast<void *>(&query)));
		unlockTable();

		if (NULL != entry) {
			result = entry->memberName;
		}
	}

	return result;
}

static UDATA
memberNameHashFn(void *key, void *userData)
{
	auto query = static_cast<MemberNameEntry *>(key);

	return query->hash;
}

static UDATA
memberNameHashEqualFn(void *leftKey, void *rightKey, void *userData)
{
	auto left = static_cast<MemberNameEntry *>(leftKey);
	auto right = static_cast<MemberNameEntry *>(rightKey);

	return left->hash == right->hash;
}

} /* end of extern "C" */
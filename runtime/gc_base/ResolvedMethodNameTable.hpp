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
 * @file
 * @ingroup GC_Base
 */

#if !defined(RESOLVED_METHOD_NAME_TABLE_HPP_)
#define RESOLVED_METHOD_NAME_TABLE_HPP_

#include "j9protos.h"

#include "BaseVirtual.hpp"

class MM_EnvironmentBase;
struct ResolvedMethodNameEntry;

class MM_ResolvedMethodNameTable : public MM_BaseVirtual
{
public:
	/**
	 * Calculate hash value as a combination of class name and method name and signature.
	 * @param className pointer to J9UTF8 class name instance
	 * @param nameAndSig pointer to J9NameAndSignature for a method
	 * @return hash value identifying the class/method combination
	 */
	UDATA calculateHash(const J9UTF8* className, const J9NameAndSignature* nameAndSig);

	/**
	 * Add a Method to the ResolvedMethodName hash table.
	 * @param vmThread pointer to J9VMThread struct
	 * @param className pointer to J9UTF8 class name instance
	 * @param nameAndSig pointer to J9NameAndSignature for a method
	 * @param method method to intern
	 * @return pointer to a ResolvedMethodName object on success, NULL on failure
	 */
	j9object_t
	intern(J9VMThread* vmThread, const J9UTF8* className, const J9NameAndSignature* nameAndSig, j9object_t method);

	/**
	 * Find a ResolvedMethodName in the hash table.
	 * @param vmThread pointer to J9VMThread struct
	 * @param className pointer to J9UTF8 class name instance
	 * @param nameAndSig pointer to J9NameAndSignature for a method
	 * @return pointer to a ResolvedMethodName object on success, NULL on failure
	 */
	j9object_t find(J9VMThread* vmThread, const J9UTF8* className, const J9NameAndSignature* nameAndSig);

	/**
	 * Retrieve ResolvedMethodName hash table.
	 * @return pointer to hash table
	 */
	J9HashTable *getTable() { return _table; }

	/**
	 * Lock the ResolvedMethodName hash table.
	 */
	void lockTable();

	/**
	 * Unlock the ResolvedMethodName hash table.
	 */
	void unlockTable();

	static MM_ResolvedMethodNameTable* newInstance(MM_EnvironmentBase* env);
	virtual void kill(MM_EnvironmentBase* env);

	MM_ResolvedMethodNameTable(MM_EnvironmentBase* env) : MM_BaseVirtual(), _table(NULL), _mutex(NULL)
	{
		_typeId = __FUNCTION__;
	}

private:
	J9HashTable* _table; /**< pointer to the hash table */
	omrthread_monitor_t _mutex; /**< hash-table mutex */

	bool initialize(MM_EnvironmentBase* env);
	void tearDown(MM_EnvironmentBase* env);
	/**
	 * Find a ResolvedMethodName in the hash table.
	 * @param vmThread pointer to J9VMThread struct
	 * @param hash hash corresponding to the ResolvedMethodName query
	 * @return pointer to a ResolvedMethodName object on success, NULL on failure
	 */
	j9object_t findInternal(J9VMThread* vmThread, UDATA hash);
};

#endif /* !defined(RESOLVED_METHOD_NAME_TABLE_HPP_) */
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

#if !defined(MEMBER_NAME_CACHE_HPP_)
#define MEMBER_NAME_CACHE_HPP_

#include "j9protos.h"

struct MemberNameEntry;

class MemberNameCache
{
public:
	/**
	 * Calculate hash value as a combination of method name and signature.
	 * @param nameAndSig pointer to J9NameAndSignature for a method
	 * @return hash value identifying the class/method combination
	 */
	UDATA calculateHash(const J9NameAndSignature *nameAndSig);

	/**
	 * Add a Method to the MemberName hash table.
	 * @param vmThread pointer to J9VMThread struct
	 * @param nameAndSig pointer to J9NameAndSignature for a method
	 * @param method method to intern
	 * @return weak pointer to a MemberName object on success, NULL on failure
	 */
	jobject intern(J9VMThread *vmThread, const J9NameAndSignature *nameAndSig, j9object_t method);

	/**
	 * Find a MemberName in the hash table.
	 * @param vmThread pointer to J9VMThread struct
	 * @param nameAndSig pointer to J9NameAndSignature for a method
	 * @return weak pointer to a MemberName object on success, NULL on failure
	 */
	jobject find(J9VMThread *vmThread, const J9NameAndSignature *nameAndSig);

	/**
	 * Retrieve MemberName hash table.
	 * @return pointer to hash table
	 */
	J9HashTable *get()
	{
		return _table;
	}

	/**
	 * Lock the MemberName hash table.
	 */
	void lockTable();

	/**
	 * Unlock the MemberName hash table.
	 */
	void unlockTable();

	static MemberNameCache *newInstance(J9VMThread *vmThread);
	virtual void kill(J9VMThread *vmThread);

	MemberNameCache() : _table(NULL), _mutex(NULL)
	{}

private:
	J9HashTable *_table; /**< pointer to the hash table */
	omrthread_monitor_t _mutex; /**< hash-table mutex */

	bool initialize();
	/**
	 * Find a MemberName in the hash table.
	 * @param vmThread pointer to J9VMThread struct
	 * @param hash hash corresponding to the MemberName query
	 * @return weak pointer to a MemberName object on success, NULL on failure
	 */
	j9object_t findInternal(J9VMThread *vmThread, UDATA hash);
};

#endif /* !defined(MEMBER_NAME_CACHE_HPP_) */

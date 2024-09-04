
/*******************************************************************************
 * Copyright IBM Corp. and others 1991
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
 * @ingroup GC_Structs
 */

#include "j9.h"
#include "j9cfg.h"
#include "j9consts.h"

#include "ClassArrayClassSlotIterator.hpp"

/**
 * @return the next slot containing an object reference
 * @return NULL if there are no more such slots
 */
J9Class *
GC_ClassArrayClassSlotIterator::nextSlot() 
{
	J9Class *classPtr = NULL;
	while (_state != classArrayClassSlotIterator_state_done) {
		switch (_state) {
		case classArrayClassSlotIterator_state_arrayClass:
			classPtr = (J9Class *)_iterateClazz->arrayClass;
			if (!_isArrayClass) {
				_state = classArrayClassSlotIterator_state_done;
			} else {
				_state += 1;
			}
			break;
		case classArrayClassSlotIterator_state_componentType:
			classPtr = ((J9ArrayClass *)_iterateClazz)->componentType;
			_state += 1;
			break;
		case classArrayClassSlotIterator_state_leafComponentType:
			classPtr = ((J9ArrayClass *)_iterateClazz)->leafComponentType;
			_state += 1;
			break;
		case classArrayClassSlotIterator_state_done:
		default:
			break;
		}
		if (NULL != classPtr) {
			break;
		}
	}
	return classPtr;
}


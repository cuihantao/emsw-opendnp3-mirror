//
// Licensed to Green Energy Corp (www.greenenergycorp.com) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  Green Enery Corp licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
#ifndef __NOTIFIER_H_
#define __NOTIFIER_H_

#include <opendnp3/APL/IEventLock.h>
#include <opendnp3/APL/INotifier.h>

namespace apl
{

template <class T>
class Notifier : public INotifier
{
public:
	Notifier(const T& arVal, IEventLock<T>* apEventLock);
	virtual ~Notifier() {}

	void Notify();

private:
	const T M_VAL;
	IEventLock<T>* mpEventLock;
};

template <class T>
Notifier<T>::Notifier(const T& arVal, IEventLock<T>* apEventLock) :
	M_VAL(arVal),
	mpEventLock(apEventLock)
{

}

template <class T>
void Notifier<T>::Notify()
{
	mpEventLock->SignalEvent(M_VAL);
}

}

#endif


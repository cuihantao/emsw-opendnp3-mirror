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
#include <opendnp3/APL/IPhysicalLayerAsync.h>
#include <opendnp3/APL/LowerLayerToPhysAdapter.h>

namespace apl
{

LowerLayerToPhysAdapter::LowerLayerToPhysAdapter(Logger* apLogger, IPhysicalLayerAsync* apPhys, bool aAutoRead) :
	Loggable(apLogger),
	IHandlerAsync(apLogger),
	ILowerLayer(apLogger),
	mAutoRead(aAutoRead),
	mNumFailure(0),
	mpPhys(apPhys)
{
	mpPhys->SetHandler(this);
}

LowerLayerToPhysAdapter::~LowerLayerToPhysAdapter()
{

}

void LowerLayerToPhysAdapter::StartRead()
{
	mpPhys->AsyncRead(mpBuff, BUFFER_SIZE);
}

/* Implement IAsyncHandler */
void LowerLayerToPhysAdapter::_OnOpenFailure()
{
	++mNumFailure;
}

void LowerLayerToPhysAdapter::_OnReadWriteFailure()
{
	++mNumFailure;
}


/* Implement IUpperLayer */
void LowerLayerToPhysAdapter::_OnReceive(const boost::uint8_t* apData, size_t aNumBytes)
{
	// process the data into another buffer *before* kicking off another call,
	// otherwise you have a potential race condition
	if(mpUpperLayer != NULL) mpUpperLayer->OnReceive(apData, aNumBytes);
	if(mAutoRead) this->StartRead();
}

void LowerLayerToPhysAdapter::_OnSendSuccess()
{
	if(mpUpperLayer != NULL) mpUpperLayer->OnSendSuccess();
}

void LowerLayerToPhysAdapter::_OnSendFailure()
{
	if(mpUpperLayer != NULL) mpUpperLayer->OnSendFailure();
}

void LowerLayerToPhysAdapter::_OnLowerLayerUp()
{
	if(mAutoRead) this->StartRead();
	if(mpUpperLayer != NULL) mpUpperLayer->OnLowerLayerUp();
}

void LowerLayerToPhysAdapter::_OnLowerLayerDown()
{
	if(mpUpperLayer != NULL) mpUpperLayer->OnLowerLayerDown();
}

void LowerLayerToPhysAdapter::_OnLowerLayerShutdown()
{

}

/* Implement ILowerLayer */
void LowerLayerToPhysAdapter::_Send(const boost::uint8_t* apData, size_t aNumBytes)
{
	mpPhys->AsyncWrite(apData, aNumBytes);
}

}//end namespace

/*
 * Licensed to Green Energy Corp (www.greenenergycorp.com) under one or more
 * contributor license agreements. See the NOTICE file distributed with this
 * work for additional information regarding copyright ownership.  Green Enery
 * Corp licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include "VtoRouter.h"

#include <boost/bind.hpp>

#include <APL/Exception.h>
#include <APL/IPhysicalLayerAsync.h>
#include <APL/Util.h>

#include "VtoReader.h"
#include "VtoRouterSettings.h"

namespace apl
{
namespace dnp
{

VtoRouter::VtoRouter(const VtoRouterSettings& arSettings, Logger* apLogger, IVtoWriter* apWriter, IPhysicalLayerAsync* apPhysLayer, ITimerSource* apTimerSrc) :
	Loggable(apLogger),
	AsyncPhysLayerMonitor(apLogger, apPhysLayer, apTimerSrc, arSettings.OPEN_RETRY_MS),
	IVtoCallbacks(arSettings.CHANNEL_ID),
	CleanupHelper(apTimerSrc),
	mpVtoWriter(apWriter),
	mReadBuffer(1024),
	mReopenPhysicalLayer(false),
	mPermanentlyStopped(false),
	mCleanedup(false)
{
	assert(apLogger != NULL);
	assert(apWriter != NULL);
	assert(apPhysLayer != NULL);
	assert(apTimerSrc != NULL);
}

void VtoRouter::StopRouter()
{
	mPermanentlyStopped = true;
	mpTimerSrc->Post(boost::bind(&VtoRouter::DoStopRouter, this));
}

void VtoRouter::DoStopRouter()
{
	this->Stop();
}

void VtoRouter::OnVtoDataReceived(const VtoData& arData)
{
	LOG_BLOCK(LEV_DEBUG, "GotRemoteData: " << arData.GetSize() << " Type: " << ToString(arData.GetType()));

	/*
	 * This will create a container object that allows us to hold the data
	 * pointer asynchronously.  We need to release the object from the queue in
	 * _OnSendSuccess().  Each call is processed serially, so we can take
	 * advantage of the FIFO structure to keep things simple.
	 */
	this->mPhysLayerTxBuffer.push(arData);
	this->CheckForPhysWrite();
}

void VtoRouter::DoStart()
{
	if(mPermanentlyStopped) {
		LOG_BLOCK(LEV_DEBUG, "Permenantly Stopped")
	}
	else {
		if(!mReopenPhysicalLayer) {
			mReopenPhysicalLayer = true;
			LOG_BLOCK(LEV_DEBUG, "Starting VtoRouted Port")
			this->Start();
		}
		else {
			LOG_BLOCK(LEV_DEBUG, "Already started")
		}
	}
}

void VtoRouter::DoStop()
{

	if(mReopenPhysicalLayer) {
		mReopenPhysicalLayer = false;
		LOG_BLOCK(LEV_DEBUG, "Stopping VtoRouted Port")
		this->Stop();
	}
	else {
		LOG_BLOCK(LEV_DEBUG, "Already stopped")
	}
}

void VtoRouter::_OnReceive(const boost::uint8_t* apData, size_t aLength)
{
	LOG_BLOCK(LEV_COMM, "GotLocalData: " << aLength);

	// turn the incoming data into a VtoMessage object and enque it
	VtoMessage msg(VTODT_DATA, apData, aLength);
	this->mVtoTxBuffer.push_back(msg);

	this->CheckForVtoWrite();
	this->CheckForPhysRead();
}

void VtoRouter::CheckForVtoWrite()
{
	// need to check mPermanentlyStopped, we will often get a physical layer closed notification after
	// we have already stopped and disposed of the dnp3 stack so we need to not call anything on mpVtoWriter
	if(!mPermanentlyStopped && !mVtoTxBuffer.empty()) {
		VtoMessage msg = mVtoTxBuffer.front();
		mVtoTxBuffer.pop_front();

		// type DATA means this is a buffer and we need to pull the data out and send it to the vto writer
		if(msg.type == VTODT_DATA) {
			size_t numWritten = mpVtoWriter->Write(msg.data.Buffer(), msg.data.Size(), this->GetChannelId());
			LOG_BLOCK(LEV_INTERPRET, "VtoWriter: " << numWritten << " of " << msg.data.Size());
			if(numWritten < msg.data.Size()) {
				size_t remainder = msg.data.Size() - numWritten;
				VtoMessage partial(VTODT_DATA, msg.data.Buffer() + numWritten, remainder);
				mVtoTxBuffer.push_front(partial);
			}
			else this->CheckForVtoWrite();
		}
		else {
			// if we have generated REMOTE_OPENED or REMOTE_CLOSED message we need to send the SetLocalVtoState
			// update to the vtowriter so it can be serialized in the correct order.
			mpVtoWriter->SetLocalVtoState(msg.type == VTODT_REMOTE_OPENED, this->GetChannelId());
			this->CheckForVtoWrite();
		}
	}

	this->CheckForPhysRead();
}

void VtoRouter::_OnSendSuccess()
{
	/*
	 * If this function has been called, it means that we can now discard the
	 * data that is at the head of the FIFO queue.
	 */
	this->mPhysLayerTxBuffer.pop();
	this->CheckForPhysWrite();
}

void VtoRouter::_OnSendFailure()
{
	/* try to re-transmit the last packet */
	this->CheckForPhysWrite();
}

void VtoRouter::CheckForPhysRead()
{
	if(mpPhys->CanRead() && mVtoTxBuffer.size() < 10) {	//TODO - Make this configurable or track the size in bytes
		mpPhys->AsyncRead(mReadBuffer, mReadBuffer.Size());
	}
}

void VtoRouter::CheckForPhysWrite()
{
	if(!mPhysLayerTxBuffer.empty()) {
		VtoDataType type = mPhysLayerTxBuffer.front().GetType();
		if(type == VTODT_DATA) {
			// only write to the physical layer if we have a valid local connection
			if(mpPhys->CanWrite()) {
				mpPhys->AsyncWrite(mPhysLayerTxBuffer.front().mpData, mPhysLayerTxBuffer.front().GetSize());
				LOG_BLOCK(LEV_COMM, "Wrote: " << mPhysLayerTxBuffer.front().GetSize());
			}
		}
		else {
			// we only want to handle the remotely sent online/offline message when are not in the process
			// of sending data (waiting for a SendSuccess or SendFailure message)
			if(!mpPhys->IsWriting()) {
				this->mPhysLayerTxBuffer.pop();
				this->DoVtoRemoteConnectedChanged(type == VTODT_REMOTE_OPENED);
			}
		}
	}
}

void VtoRouter::OnBufferAvailable()
{
	this->CheckForVtoWrite();
}

void VtoRouter::OnPhysicalLayerOpen()
{
	this->SetLocalConnected(true);

	this->CheckForPhysRead();
	this->CheckForPhysWrite();
	this->CheckForVtoWrite();
}

void VtoRouter::OnStateChange(PhysLayerState aState)
{
	if(mPermanentlyStopped && aState == PLS_STOPPED && !mCleanedup) {
		mCleanedup = true;
		this->Cleanup();
	}
}

void VtoRouter::OnPhysicalLayerClose()
{
	this->SetLocalConnected(false);
	this->CheckForPhysWrite();
	this->CheckForVtoWrite();
}

void VtoRouter::OnPhysicalLayerOpenFailure()
{
	this->SetLocalConnected(false);
	this->CheckForPhysWrite();
	this->CheckForVtoWrite();
}

}
}

/* vim: set ts=4 sw=4: */


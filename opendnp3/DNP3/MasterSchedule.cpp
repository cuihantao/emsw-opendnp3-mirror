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

#include <opendnp3/APL/AsyncTaskBase.h>
#include <opendnp3/APL/AsyncTaskContinuous.h>
#include <opendnp3/APL/AsyncTaskGroup.h>
#include <opendnp3/APL/AsyncTaskPeriodic.h>
#include <opendnp3/DNP3/Master.h>
#include <opendnp3/DNP3/MasterSchedule.h>

#include <boost/foreach.hpp>

namespace apl
{
namespace dnp
{

MasterSchedule::MasterSchedule(AsyncTaskGroup* apGroup, Master* apMaster, const MasterConfig& arCfg) :
	mpGroup(apGroup),
	mTracking(apGroup)
{
	this->Init(arCfg, apMaster);
}

void MasterSchedule::EnableOnlineTasks()
{
	mpGroup->Enable(ONLINE_ONLY_TASKS);
}

void MasterSchedule::DisableOnlineTasks()
{
	mpGroup->Disable(ONLINE_ONLY_TASKS);
}

void MasterSchedule::ResetStartupTasks()
{
	mpGroup->ResetTasks(START_UP_TASKS);
}

void MasterSchedule::AddOnDemandIntegrityPoll(Master *apMaster)
{
	/* Schedule On Demand Integrity Poll */
	AsyncTaskBase* pIntegrity = mTracking.Add(
	                               -1,	// Non periodic task
	                               0,	// No retry on failure
	                               AMP_POLL,
	                               bind(&Master::IntegrityPoll, apMaster, _1),
	                               "On-Demand Integrity Poll");

	pIntegrity->SetFlags(ONLINE_ONLY_TASKS);

	/* Enable task execution */
	pIntegrity->Enable();

	return;
}

void MasterSchedule::Init(const MasterConfig& arCfg, Master* apMaster)
{
	/*
	 * Create exception scan tasks and chain their dependency. We keep track of the
	 * first and the last.
	 */
	AsyncTaskBase* pFirstEventScan = NULL;
	AsyncTaskBase* pLastEventScan = NULL;
	BOOST_FOREACH(ExceptionScan e, arCfg.mScans) {
		AsyncTaskBase* pEventScan = mTracking.Add(
		                                e.ScanRate,
		                                arCfg.TaskRetryRate,
		                                AMP_POLL,
		                                bind(&Master::EventPoll, apMaster, _1, e.ClassMask),
		                                "Event Scan");

		pEventScan->SetFlags(ONLINE_ONLY_TASKS | START_UP_TASKS);

		if (pLastEventScan) {
			pEventScan->AddDependency(pLastEventScan);
			pLastEventScan = pEventScan;
		}
		else {
			pFirstEventScan = pEventScan;
			pLastEventScan = pEventScan;
		}
	}

	// Create integrity poll task
	AsyncTaskBase* pIntegrity = mTracking.Add(
	                                arCfg.IntegrityRate,
	                                arCfg.TaskRetryRate,
	                                AMP_POLL,
	                                bind(&Master::IntegrityPoll, apMaster, _1),
	                                "Integrity Poll");

	pIntegrity->SetFlags(ONLINE_ONLY_TASKS | START_UP_TASKS);

	if (arCfg.DoUnsolOnStartup) {
		/*
		 * DNP3Spec-V2-Part2-ApplicationLayer-_20090315.pdf, page 8
		 * says that UNSOL should be disabled before an integrity scan
		 * is done.
		 */
		TaskHandler handler = bind(&Master::ChangeUnsol,
		                           apMaster,
		                           _1,
		                           false,
		                           PC_ALL_EVENTS);
		AsyncTaskBase* pUnsolDisable = mTracking.Add(
		                                   -1,
		                                   arCfg.TaskRetryRate,
		                                   AMP_UNSOL_CHANGE,
		                                   handler,
		                                   "Unsol Disable");

		pUnsolDisable->SetFlags(ONLINE_ONLY_TASKS | START_UP_TASKS);

		/*
		 * If we have exception scan task(s), then make the first be dependent
		 * on the unsol disable task and the integrity poll task be dependent
		 * on the last. This is done so that the exception scan can pick up point change
		 * events with timestamp before an integrity poll.
		 *
		 * If there is no exception scan, then make the integrity poll task be
		 * next in the series after the unsolicited disable task.
		 */
		if (pFirstEventScan) {
			pFirstEventScan->AddDependency(pUnsolDisable);
			pIntegrity->AddDependency(pLastEventScan);
		}
		else {
			pIntegrity->AddDependency(pUnsolDisable);
		}

		if (arCfg.EnableUnsol) {
			TaskHandler handler = bind(
			                          &Master::ChangeUnsol,
			                          apMaster,
			                          _1,
			                          true,
			                          arCfg.UnsolClassMask);

			AsyncTaskBase* pUnsolEnable = mTracking.Add(
			                                  -1,
			                                  arCfg.TaskRetryRate,
			                                  AMP_UNSOL_CHANGE,
			                                  handler,
			                                  "Unsol Enable");

			pUnsolEnable->SetFlags(ONLINE_ONLY_TASKS | START_UP_TASKS);
			pUnsolEnable->AddDependency(pIntegrity);
		}
	}

	/* Tasks are executed when the master is is idle */
	mpCommandTask = mTracking.AddContinuous(
	                    AMP_COMMAND,
	                    boost::bind(&Master::ProcessCommand, apMaster, _1),
	                    "Command");

	mpTimeTask = mTracking.AddContinuous(
	                 AMP_TIME_SYNC,
	                 boost::bind(&Master::SyncTime, apMaster, _1),
	                 "TimeSync");

	mpClearRestartTask = mTracking.AddContinuous(
	                         AMP_CLEAR_RESTART,
	                         boost::bind(&Master::WriteIIN, apMaster, _1),
	                         "Clear IIN");

	mpVtoTransmitTask = mTracking.AddContinuous(
	                        AMP_VTO_TRANSMIT,
	                        boost::bind(&Master::TransmitVtoData, apMaster, _1),
	                        "Buffer VTO Data");

	mpCommandTask->SetFlags(ONLINE_ONLY_TASKS);
	mpVtoTransmitTask->SetFlags(ONLINE_ONLY_TASKS);
	mpTimeTask->SetFlags(ONLINE_ONLY_TASKS);
	mpClearRestartTask->SetFlags(ONLINE_ONLY_TASKS);
}

void MasterSchedule::AdjustIntegrityPollRate(millis_t IntegrityRate)
{
	AsyncTaskBase *aTask = NULL;

	aTask = mTracking.FindTaskByName("Integrity Poll");

	if (NULL != aTask) {
		AsyncTaskPeriodic* aPer = dynamic_cast<AsyncTaskPeriodic*>(aTask);
		/* Check to see if aPer is NULL to see if dynamic cast worked */
		if (NULL != aPer) {
			aPer->UpdatePeriod(IntegrityRate);
		}
	}

}

}
}

/* vim: set ts=4 sw=4: */


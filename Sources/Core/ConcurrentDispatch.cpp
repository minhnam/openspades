/*
 Copyright (c) 2013 yvt
 
 This file is part of OpenSpades.
 
 OpenSpades is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 OpenSpades is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with OpenSpades.  If not, see <http://www.gnu.org/licenses/>.
 
 */

#include <OpenSpades.h>
#include "ConcurrentDispatch.h"
#include "../Imports/SDL.h"
#include "Mutex.h"
#include <list>
#include "AutoLocker.h"
#include "Debug.h"
#include "Exception.h"
#include "Thread.h"
#include "Settings.h"
#include <sys/types.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#else
#if defined(WIN32)
#include <windows.h>
#else
#ifndef _MSC_VER
#include <unistd.h>
#endif
#if defined(__GNUC__)
#include <sys/sysinfo.h>
#endif
#endif
#endif

#include "ThreadLocalStorage.h"

SPADES_SETTING(core_numDispatchQueueThreads, "auto");

static int GetNumCores() {
#ifdef WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
#elif defined(__APPLE__)
    int nm[2];
    size_t len = 4;
    uint32_t count;
	
    nm[0] = CTL_HW; nm[1] = HW_AVAILCPU;
    sysctl(nm, 2, &count, &len, NULL, 0);
	
    if(count < 1) {
        nm[1] = HW_NCPU;
        sysctl(nm, 2, &count, &len, NULL, 0);
        if(count < 1) { count = 1; }
    }
    return count;
#elif defined(__GNUC__)
    return get_nprocs();
#else
    return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

namespace spades {
	
	struct SyncQueueEntry{
		SDL_cond *doneCond;
		SDL_mutex *doneMutex;
		ConcurrentDispatch *dispatch;
		volatile bool done;
		volatile bool released;
		
		SyncQueueEntry(ConcurrentDispatch *disp):
		doneCond(SDL_CreateCond()),
		doneMutex(SDL_CreateMutex()),
		dispatch(disp),
		done(false),
		released(false){
		}
		~SyncQueueEntry() {
			SDL_DestroyCond(doneCond);
			SDL_DestroyMutex(doneMutex);
			if(released){
				dispatch->entry = NULL;
				delete dispatch;
			}
		}
		
		void Done() {
			SDL_LockMutex(doneMutex);
			done = true;
			SDL_CondBroadcast(doneCond);
			if(released){
				delete this;
				return;
			}
			SDL_UnlockMutex(doneMutex);
		}
		
		void Release() {
			SDL_LockMutex(doneMutex);
			released = true;
			if(done){
				delete this;
				return;
			}
			SDL_UnlockMutex(doneMutex);
		}
		
		void Join(){
			SDL_LockMutex(doneMutex);
			while(!done){
				SDL_CondWait(doneCond, doneMutex);
			}
			SDL_UnlockMutex(doneMutex);
		}
	};
	
	class SynchronizedQueue {
		std::list<SyncQueueEntry *> entries;
		
		SDL_cond *pushCond;
		SDL_mutex *pushMutex;
	public:
		SynchronizedQueue() {
			pushMutex = SDL_CreateMutex();
			pushCond = SDL_CreateCond();
		}
		~SynchronizedQueue() {
			SDL_DestroyMutex(pushMutex);
			SDL_DestroyCond(pushCond);
		}
		
		void Push(SyncQueueEntry * entry) {
			SDL_LockMutex(pushMutex);
			try{
				entries.push_back(entry);
			}catch(...){
				SDL_UnlockMutex(pushMutex);
				throw;
			}
			SDL_CondSignal(pushCond);
			SDL_UnlockMutex(pushMutex);
		}
		
		SyncQueueEntry *Wait() {
			SDL_LockMutex(pushMutex);
			while(entries.empty()){
				SDL_CondWait(pushCond, pushMutex);
			}
			
			SyncQueueEntry *ent = entries.front();
			entries.pop_front();
			SDL_UnlockMutex(pushMutex);
			
			return ent;
		}
		
		SyncQueueEntry *Poll(){
			SDL_LockMutex(pushMutex);
			if(!entries.empty()){
				SyncQueueEntry *ent = entries.front();
				entries.pop_front();
				SDL_UnlockMutex(pushMutex);
				return ent;
			}
			SDL_UnlockMutex(pushMutex);
			return NULL;
		}
	};
	
	static SynchronizedQueue globalQueue;
	static AutoDeletedThreadLocalStorage<DispatchQueue> threadQueue("threadDispatchQueue");
	static DispatchQueue *sdlQueue = NULL;
	
	DispatchQueue::DispatchQueue(){
		SPADES_MARK_FUNCTION();
		internal = new SynchronizedQueue();
	}
	DispatchQueue::~DispatchQueue(){
		SPADES_MARK_FUNCTION();
		delete internal;
	}
	
	DispatchQueue *DispatchQueue::GetThreadQueue() {
		SPADES_MARK_FUNCTION();
		DispatchQueue *q = threadQueue;
		if(!q){
			q = new DispatchQueue();
			threadQueue = q;
		}
		return q;
	}
	
	void DispatchQueue::ProcessQueue() {
		SPADES_MARK_FUNCTION();
		SyncQueueEntry *ent;
		while((ent = internal->Poll()) != NULL){
			ent->dispatch->Execute();
		}
		Thread::CleanupExitedThreads();
	}
	
	void DispatchQueue::EnterEventLoop() throw() {
		while(true){
			SyncQueueEntry *ent = internal->Wait();
			ent->dispatch->ExecuteProtected();
			
		}
	}
	
	void DispatchQueue::MarkSDLVideoThread() {
		sdlQueue = this;
	}
	
	class DispatchThread: public Thread{
	public:
		virtual void Run() throw() {
			SPADES_MARK_FUNCTION();
			while(true){
				SyncQueueEntry *ent = globalQueue.Wait();
				ent->dispatch->ExecuteProtected();
			}
		}
	};
	
	static std::vector<DispatchThread *> threads;
	
	ConcurrentDispatch::ConcurrentDispatch():
	entry(NULL), runnable(NULL){
		SPADES_MARK_FUNCTION();
	}
	ConcurrentDispatch::ConcurrentDispatch(std::string name):
	entry(NULL),name(name), runnable(NULL){
		SPADES_MARK_FUNCTION();
	}
	
	ConcurrentDispatch::~ConcurrentDispatch(){
		SPADES_MARK_FUNCTION();
		Join();
	}
	
	void ConcurrentDispatch::Execute() {
		SPADES_MARK_FUNCTION();
		SyncQueueEntry *ent = entry;
		if(!ent){
			SPRaise("Attempted to execute dispatch '%s' without entry", name.c_str());
		}
		try{
			Run();
		}catch(...){
			ent->Done();
			throw;
		}
		ent->Done();
	}
	
	void ConcurrentDispatch::ExecuteProtected() throw() {
		try{
			Execute();
		}catch(const std::exception& ex){
			fprintf(stderr, "-- UNHANDLED CONCURRENT DISPATCH EXCEPTION ---\n");
			fprintf(stderr, "%s\n", ex.what());
		}catch(...){
			fprintf(stderr, "-- UNHANDLED CONCURRENT DISPATCH EXCEPTION ---\n");
			fprintf(stderr, "(no information provided)\n");
		}
	}
	
	void ConcurrentDispatch::Start() {
		SPADES_MARK_FUNCTION();
		if(entry){
			SPRaise("Attempted to start dispatch '%s' when it's already started", name.c_str());
		}else{
			if(threads.empty()){
				int cnt = GetNumCores();
				if(!("auto" == core_numDispatchQueueThreads)){
					cnt = core_numDispatchQueueThreads;
				}
				SPLog("Creating %d dispatch thread(s)",
					  cnt);
				for(int i = 0; i < cnt; i++){
					DispatchThread *t = new DispatchThread();
					threads.push_back(t);
					t->Start();
				}
			}
			entry = new SyncQueueEntry(this);
			globalQueue.Push(entry);
		}
	}
	
	void ConcurrentDispatch::StartOn(DispatchQueue *queue) {
		SPADES_MARK_FUNCTION();
		if(entry){
			SPRaise("Attempted to start dispatch '%s' when it's already started", name.c_str());
		}else{
			entry = new SyncQueueEntry(this);
			queue->internal->Push(entry);
			
			if(queue == sdlQueue) {
				SDL_Event evt;
				memset(&evt, 0, sizeof(evt));
				evt.type = SDL_USEREVENT;
				SDL_PushEvent(&evt);
			}
		}
	}
	
	void ConcurrentDispatch::Join() {
		SPADES_MARK_FUNCTION();
		if(!entry){
		}else{
			entry->Join();
			delete entry;
			entry = NULL;
		}
	}
	
	void ConcurrentDispatch::Release(){
		SPADES_MARK_FUNCTION();
		if(entry){
			SyncQueueEntry * ent = entry;
			ent->Release();
		}
	}
	
	void ConcurrentDispatch::Run(){
		if(runnable)
			runnable->Run();
	}
}

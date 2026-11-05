#pragma once

#include "Async/Async.h"
#include "CoreMinimal.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

THIRD_PARTY_INCLUDES_START
#include <tbb/parallel_for.h>
THIRD_PARTY_INCLUDES_END

namespace ProcessAsync
{
	template<typename T>
	TArray<T> ProcessAsync(int32 NumInput, int32 ThreadPointNum, TFunction<T(int32)> Func)
	{
		int32 NumThreads = FMath::Min(NumInput / ThreadPointNum, FPlatformMisc::NumberOfCoresIncludingHyperthreads() - 1LL);
		TArray<TFuture<TArray<T>>> Threads;
		Threads.Reserve(NumThreads);
		const int32 Batch = NumInput / NumThreads;
		TArray<T> Results;
		Results.Reserve(NumThreads);
		if (NumThreads == 0)
		{
			return Results;
		}
		for (int32 t = 0; t < NumThreads; ++t)
		{
			const int64 StartIdx = Batch * t;
			const int64 EndIdx = t == NumThreads - 1 ? NumInput : StartIdx + Batch;
			Threads.Emplace(Async(EAsyncExecution::TaskGraph, [StartIdx, EndIdx, Func]
			{
				TArray<T> ResultsPerTask;
				ResultsPerTask.Reserve(EndIdx - StartIdx);
				for (int64 p = StartIdx; p < EndIdx; ++p)
				{
					T Results = Func(p);
					ResultsPerTask.Add(Results);
				}
				//FPlatformProcess::Sleep(10);
				return ResultsPerTask;
			}));
		}

		for (const TFuture<TArray<T>>& ThreadResult : Threads)
		{
			ThreadResult.Wait();
			TArray<T> ResultsPerTask =  ThreadResult.Get();
			Results.Append(ResultsPerTask);
		}
		return Results;
	}
	
}
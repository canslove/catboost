#pragma once

#include "application_options.h"
#include "train_options.h"
#include "json_options.h"
#include "model_helpers.h"
#include <catboost/cuda/data/load_config.h>
#include <catboost/cuda/data/load_data.h>
#include <catboost/cuda/methods/boosting.h>
#include <catboost/cuda/targets/mse.h>
#include <catboost/cuda/methods/oblivious_tree.h>
#include <catboost/cuda/targets/cross_entropy.h>
#include <catboost/cuda/data/cat_feature_perfect_hash.h>
#include <catboost/cuda/gpu_data/pinned_memory_estimation.h>
#include <catboost/cuda/cpu_compatibility_helpers/model_converter.h>
#include <catboost/cuda/cpu_compatibility_helpers/full_model_saver.h>
#include <catboost/cuda/cpu_compatibility_helpers/cpu_pool_based_data_provider_builder.h>
#include <catboost/libs/model/model.h>
#include <util/system/fs.h>

namespace NCatboostCuda
{
    inline void UpdatePinnedMemorySizeOption(const TDataProvider& learn,
                                             const TDataProvider* test,
                                             const TBinarizedFeaturesManager& featuresManager,
                                             TTrainCatBoostOptions& catBoostOptions)
    {
        if (catBoostOptions.BoostingOptions.UseCpuRamForCatFeaturesDataSet())
        {
            ui32 devCount = catBoostOptions.ApplicationOptions.GetDeviceCount();
            ui32 additionalBytes =
                    1.05 * EstimatePinnedMemorySizeInBytesPerDevice(learn, test, featuresManager, devCount);
            catBoostOptions.ApplicationOptions.GetCudaApplicationConfig().PinnedMemorySize += additionalBytes;
        }
    }

    inline bool HasCtrs(const TBinarizedFeaturesManager& featuresManager)
    {
        for (auto catFeature : featuresManager.GetCatFeatureIds())
        {
            if (featuresManager.UseForCtr(catFeature) || featuresManager.UseForTreeCtr(catFeature))
            {
                return true;
            }
        }
        return false;
    }

    inline void UpdateOptionsAndEnableCtrTypes(TTrainCatBoostOptions& options,
                                               TBinarizedFeaturesManager& featuresManager)
    {
        if (options.TargetOptions.GetTargetType() == ETargetFunction::RMSE)
        {
            options.TreeConfig.SetLeavesEstimationIterations(1);
        }

        if (options.TargetOptions.GetTargetType() == ETargetFunction::CrossEntropy)
        {
            options.FeatureManagerOptions.SetTargetBinarization(2);
        }
        if (options.FeatureManagerOptions.IsCtrTypeEnabled(ECtrType::FeatureFreq))
        {
            yvector<float> prior = {0.5f};
            featuresManager.EnableCtrType(ECtrType::FeatureFreq, prior);
        }

        const bool isFloatTargetMeanCtrEnabled = (!options.FeatureManagerOptions.IsCustomCtrTypes() &&
                                                  options.TargetOptions.GetTargetType() == ETargetFunction::RMSE) ||
                                                 options.FeatureManagerOptions.IsCtrTypeEnabled(
                                                         ECtrType::FloatTargetMeanValue);
        if (isFloatTargetMeanCtrEnabled)
        {
            yvector<float> prior = {0.0, 3.0};
            featuresManager.EnableCtrType(ECtrType::FloatTargetMeanValue, prior);
        }

        if (options.TargetOptions.GetTargetType() == ETargetFunction::RMSE)
        {
            yvector<float> prior = {0.5f};
            if (options.FeatureManagerOptions.IsCtrTypeEnabled(ECtrType::Borders))
            {
                featuresManager.EnableCtrType(ECtrType::Borders, prior);
            }
            if (options.FeatureManagerOptions.IsCtrTypeEnabled(ECtrType::Buckets))
            {
                featuresManager.EnableCtrType(ECtrType::Buckets, prior);
            }
        } else
        {
            if (options.FeatureManagerOptions.IsCtrTypeEnabled(ECtrType::Borders))
            {
                MATRIXNET_WARNING_LOG
                << "Warning: borders ctr aren't supported for target " << options.TargetOptions.GetTargetType()
                << ". Change type for buckets" << Endl;
                options.FeatureManagerOptions.DisableCtrType(ECtrType::Borders);
                options.FeatureManagerOptions.EnableCtrType(ECtrType::Buckets);
            }
            if (options.FeatureManagerOptions.IsCtrTypeEnabled(ECtrType::Buckets))
            {
                yvector<float> prior = {0.5, 0.5};
                featuresManager.EnableCtrType(ECtrType::Buckets, prior);

                prior = {1.0, 0.0};
                featuresManager.EnableCtrType(ECtrType::Buckets, prior);

                prior = {0.0, 1.0};
                featuresManager.EnableCtrType(ECtrType::Buckets, prior);
            }
        }

        //don't make several permutations in matrixnet-like mode if we don't have ctrs
        if (!HasCtrs(featuresManager) && options.BoostingOptions.GetBoostingType() == EBoostingType::Plain)
        {
            if (options.BoostingOptions.GetPermutationCount() > 1)
            {
                MATRIXNET_INFO_LOG
                << "No catFeatures for ctrs found and don't look ahead is disabled. Fallback to one permutation"
                << Endl;
            }
            options.BoostingOptions.SetPermutationCount(1);
        }
    }

    template<template<class TMapping, class> class TTargetTemplate, NCudaLib::EPtrType CatFeaturesStoragePtrType>
    inline THolder<TAdditiveModel<TObliviousTreeModel>> Train(TBinarizedFeaturesManager& featureManager,
                                                              const TBoostingOptions& boostingOptions,
                                                              const TOutputFilesOptions& logOptions,
                                                              const TObliviousTreeLearnerOptions& treeOptions,
                                                              const TTargetOptions& targetOptions,
                                                              const TDataProvider& learn,
                                                              const TDataProvider* test,
                                                              TRandom& random) {
        using TTaskDataSet = TDataSet<CatFeaturesStoragePtrType>;
        using TTarget = TTargetTemplate<NCudaLib::TMirrorMapping, TTaskDataSet>;

        TObliviousTree tree(featureManager, treeOptions);
        TDontLookAheadBoosting<TTargetTemplate, TObliviousTree, CatFeaturesStoragePtrType> boosting(featureManager,
                                                                                                    boostingOptions,
                                                                                                    targetOptions,
                                                                                                    random,
                                                                                                    tree);
        boosting.SetDataProvider(learn, test);

        using TMetricPrinter = TMetricLogger<TTarget, TObliviousTreeModel>;
        TOFStream meta(logOptions.GetMetaFile());
        TIterationLogger<TTarget, TObliviousTreeModel> iterationPrinter;
        TTimeWriter<TTarget, TObliviousTreeModel> timeWriter(boostingOptions.GetIterationCount(),
                                                             logOptions.GetTimeLeftLog());

        THolder<IOverfittingDetector> overfitDetector;

        boosting.RegisterLearnListener(iterationPrinter);
        boosting.RegisterLearnListener(timeWriter);

        THolder<TMetricPrinter> learnPrinter;
        THolder<TMetricPrinter> testPrinter;

        meta << "name\t" << logOptions.GetName() << Endl;
        meta << "iterCount\t" << boostingOptions.GetIterationCount() << Endl;

        if (boostingOptions.IsCalcScores())
        {
            learnPrinter.Reset(new TMetricPrinter("Learn score: ", logOptions.GetLearnErrorLogPath()));
            //output log files path relative to trainDirectory
            meta << "learnErrorLog\t" << logOptions.GetLearnErrorLogPath() << Endl;
            if (test)
            {
                testPrinter.Reset(new TMetricPrinter("Test score: ", logOptions.GetTestErrorLogPath()));
                meta << "testErrorLog\t" << logOptions.GetTestErrorLogPath() << Endl;

                const auto& odOptions = boostingOptions.GetOverfittingDetectorOptions();
                if (odOptions.GetAutoStopPval() > 0)
                {
                    overfitDetector = odOptions.CreateOverfittingDetector(!TTarget::IsMinOptimal());
                    testPrinter->RegisterOdDetector(overfitDetector.Get());
                }
            }
        }
        meta << "timeLeft\t" << logOptions.GetTimeLeftLog() << Endl;
        meta << "loss\t" << TMetricPrinter::GetMetricName() << "\t" << (TMetricPrinter::IsMinOptimal() ? "min" : "max")
             << Endl;

        if (learnPrinter)
        {
            boosting.RegisterLearnListener(*learnPrinter);
        }

        if (testPrinter)
        {
            boosting.RegisterTestListener(*testPrinter);
        }
        if (overfitDetector)
        {
            boosting.AddOverfitDetector(*overfitDetector);
        }
        auto model = boosting.Run();
        if (boostingOptions.UseBestModel())
        {
            if (testPrinter == nullptr)
            {
                MATRIXNET_INFO_LOG << "Warning: can't use-best-model without test set. Will skip model shrinking";
            } else
            {
                CB_ENSURE(testPrinter);
                const ui32 bestIter = testPrinter->GetBestIteration();
                model->Shrink(bestIter);
            }
        }
        return model;
    }

    template<template<class TMapping, class> class TTargetTemplate>
    inline THolder<TAdditiveModel<TObliviousTreeModel>> Train(TBinarizedFeaturesManager& featureManager,
                                                              const TBoostingOptions& boostingOptions,
                                                              const TOutputFilesOptions& outputFilesOptions,
                                                              const TObliviousTreeLearnerOptions& treeOptions,
                                                              const TTargetOptions& targetOptions,
                                                              const TDataProvider& learn,
                                                              const TDataProvider* test,
                                                              TRandom& random,
                                                              bool storeCatFeaturesInPinnedMemory)
    {
        if (storeCatFeaturesInPinnedMemory)
        {
            return Train<TTargetTemplate, NCudaLib::CudaHost>(featureManager, boostingOptions, outputFilesOptions,
                                                              treeOptions, targetOptions, learn, test, random);
        } else
        {
            return Train<TTargetTemplate, NCudaLib::CudaDevice>(featureManager, boostingOptions, outputFilesOptions,
                                                                treeOptions, targetOptions, learn, test, random);
        }
    };

    TCoreModel TrainModel(const TTrainCatBoostOptions trainCatBoostOptions,
                          const TDataProvider& dataProvider,
                          const TDataProvider* testProvider,
                          TBinarizedFeaturesManager& featuresManager)
    {
        TCoreModel result;
        NCudaLib::SetApplicationConfig(trainCatBoostOptions.ApplicationOptions.GetCudaApplicationConfig());
        StartCudaManager();
        {
            if (NCudaLib::GetCudaManager().GetDeviceCount() > 1)
            {
                NCudaLib::GetLatencyAndBandwidthStats<NCudaLib::CudaDevice, NCudaLib::CudaHost>();
                NCudaLib::GetLatencyAndBandwidthStats<NCudaLib::CudaDevice, NCudaLib::CudaDevice>();
                NCudaLib::GetLatencyAndBandwidthStats<NCudaLib::CudaHost, NCudaLib::CudaDevice>();
            }
            auto& profiler = NCudaLib::GetCudaManager().GetProfiler();

            if (trainCatBoostOptions.ApplicationOptions.IsProfile())
            {
                profiler.SetDefaultProfileMode(NCudaLib::EProfileMode::ImplicitLabelSync);
            } else
            {
                profiler.SetDefaultProfileMode(NCudaLib::EProfileMode::NoProfile);
            }
            TRandom random(trainCatBoostOptions.TreeConfig.GetBootstrapConfig().GetSeed());

            THolder<TAdditiveModel<TObliviousTreeModel>> model;
            const bool storeCatFeaturesInPinnedMemory = trainCatBoostOptions.BoostingOptions.UseCpuRamForCatFeaturesDataSet();

            switch (trainCatBoostOptions.TargetOptions.GetTargetType())
            {
                case ETargetFunction::RMSE:
                {
                    model = Train<TL2>(featuresManager,
                                       trainCatBoostOptions.BoostingOptions,
                                       trainCatBoostOptions.OutputFilesOptions,
                                       trainCatBoostOptions.TreeConfig,
                                       trainCatBoostOptions.TargetOptions,
                                       dataProvider,
                                       testProvider,
                                       random,
                                       storeCatFeaturesInPinnedMemory);
                    break;
                }
                case ETargetFunction::CrossEntropy:
                case ETargetFunction::Logloss:
                {
                    model = Train<TCrossEntropy>(featuresManager,
                                                 trainCatBoostOptions.BoostingOptions,
                                                 trainCatBoostOptions.OutputFilesOptions,
                                                 trainCatBoostOptions.TreeConfig,
                                                 trainCatBoostOptions.TargetOptions,
                                                 dataProvider, testProvider, random,
                                                 storeCatFeaturesInPinnedMemory);
                    break;
                }
            }

            result = ConvertToCoreModel(featuresManager,
                                        dataProvider,
                                        *model);
            {
                NJson::TJsonValue options(NJson::EJsonValueType::JSON_MAP);
                TOptionsJsonConverter<TTrainCatBoostOptions>::Save(trainCatBoostOptions, options);
                result.ModelInfo["params"] = ToString(options);
            }
        }
        StopCudaManager();
        return result;
    }


    void TrainModel(const NJson::TJsonValue& params,
                    TPool& learnPool,
                    const TPool& testPool,
                    const TString& outputModelPath,
                    TFullModel* model)
    {

        TTrainCatBoostOptions catBoostOptions;
        TDataProvider dataProvider;
        THolder<TDataProvider> testData;
        if (testPool.Docs.GetDocCount())
        {
            testData = MakeHolder<TDataProvider>();
        }

        TOptionsJsonConverter<TTrainCatBoostOptions>::Load(params, catBoostOptions);
        TBinarizedFeaturesManager featuresManager(catBoostOptions.FeatureManagerOptions);

        {

            CB_ENSURE(learnPool.Docs.GetDocCount(), "Error: empty learn pool");
            TCpuPoolBasedDataProviderBuilder builder(featuresManager, learnPool, false, dataProvider);
            builder.AddIgnoredFeatures(catBoostOptions.FeatureManagerOptions.GetIgnoredFeatures())
                    .Finish(catBoostOptions.ApplicationOptions.GetNumThreads());
        }
        if (testData != nullptr)
        {
            TCpuPoolBasedDataProviderBuilder builder(featuresManager, testPool, true, *testData);
            builder.AddIgnoredFeatures(catBoostOptions.FeatureManagerOptions.GetIgnoredFeatures())
                    .Finish(catBoostOptions.ApplicationOptions.GetNumThreads());
        }

        UpdatePinnedMemorySizeOption(dataProvider, testData.Get(), featuresManager, catBoostOptions);
        UpdateOptionsAndEnableCtrTypes(catBoostOptions, featuresManager);

        auto coreModel = TrainModel(catBoostOptions, dataProvider, testData.Get(), featuresManager);

        if (outputModelPath.Size()) {
            MakeFullModel(coreModel,
                          learnPool,
                          catBoostOptions.ApplicationOptions.GetNumThreads(),
                          outputModelPath);
        } else
        {
            CB_ENSURE(model, "Error: Model and output path are empty");
            MakeFullModel(coreModel,
                          learnPool,
                          catBoostOptions.ApplicationOptions.GetNumThreads(),
                          model);
        }
    }
}

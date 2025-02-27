/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder.h"

#include <fmt/format.h>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/match_path.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/column_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/values/sort_spec.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/match_expression_dependencies.h"
#include "mongo/db/pipeline/abt/field_map_builder.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/query/bind_input_params.h"
#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_coll_scan.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_index_scan.h"
#include "mongo/db/query/sbe_stage_builder_projection.h"
#include "mongo/db/query/shard_filterer_factory_impl.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stage_builder {
namespace {
/**
 * Generates an EOF plan. Note that even though this plan will return nothing, it will still define
 * the slots specified by 'reqs'.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateEofPlan(
    PlanNodeId nodeId, const PlanStageReqs& reqs, sbe::value::SlotIdGenerator* slotIdGenerator) {
    PlanStageSlots outputs(reqs, slotIdGenerator);

    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;
    auto slots = getSlotsToForward(reqs, outputs);
    for (auto&& slot : slots) {
        projects.insert({slot, sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0)});
    }

    auto stage = sbe::makeS<sbe::LimitSkipStage>(
        sbe::makeS<sbe::CoScanStage>(nodeId), 0, boost::none, nodeId);

    if (!projects.empty()) {
        // Even though this SBE tree will produce zero documents, we still need a ProjectStage to
        // define the slots in 'outputSlots' so that calls to getAccessor() won't fail.
        stage = sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), nodeId);
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace

std::unique_ptr<sbe::RuntimeEnvironment> makeRuntimeEnvironment(
    const CanonicalQuery& cq,
    OperationContext* opCtx,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    auto env = std::make_unique<sbe::RuntimeEnvironment>();

    // Register an unowned global timezone database for datetime expression evaluation.
    env->registerSlot("timeZoneDB"_sd,
                      sbe::value::TypeTags::timeZoneDB,
                      sbe::value::bitcastFrom<const TimeZoneDatabase*>(getTimeZoneDatabase(opCtx)),
                      false,
                      slotIdGenerator);

    if (auto collator = cq.getCollator(); collator) {
        env->registerSlot("collator"_sd,
                          sbe::value::TypeTags::collator,
                          sbe::value::bitcastFrom<const CollatorInterface*>(collator),
                          false,
                          slotIdGenerator);
    }

    for (auto&& [id, name] : Variables::kIdToBuiltinVarName) {
        if (id != Variables::kRootId && id != Variables::kRemoveId &&
            cq.getExpCtx()->variables.hasValue(id)) {
            auto [tag, val] = makeValue(cq.getExpCtx()->variables.getValue(id));
            env->registerSlot(name, tag, val, true, slotIdGenerator);
        } else if (id == Variables::kSearchMetaId) {
            // Normally, $search is responsible for setting a value for SEARCH_META, in which case
            // we will bind the value to a slot above. However, in the event of a query that does
            // not use $search, but references SEARCH_META, we need to bind a value of 'missing' to
            // a slot so that the plan can run correctly.
            env->registerSlot(name, sbe::value::TypeTags::Nothing, 0, false, slotIdGenerator);
        }
    }

    return env;
}

sbe::value::SlotVector getSlotsToForward(const PlanStageReqs& reqs,
                                         const PlanStageSlots& outputs,
                                         const sbe::value::SlotVector& exclude) {
    auto excludeSet = sbe::value::SlotSet{exclude.begin(), exclude.end()};

    std::vector<std::pair<PlanStageSlots::Name, sbe::value::SlotId>> pairs;
    outputs.forEachSlot(reqs, [&](auto&& slot, const PlanStageSlots::Name& name) {
        if (!excludeSet.count(slot)) {
            pairs.emplace_back(name, slot);
        }
    });
    std::sort(pairs.begin(), pairs.end());

    auto outputSlots = sbe::makeSV();
    for (auto&& p : pairs) {
        outputSlots.emplace_back(p.second);
    }
    return outputSlots;
}

void prepareSlotBasedExecutableTree(OperationContext* opCtx,
                                    sbe::PlanStage* root,
                                    PlanStageData* data,
                                    const CanonicalQuery& cq,
                                    const MultipleCollectionAccessor& collections,
                                    PlanYieldPolicySBE* yieldPolicy,
                                    const bool preparingFromCache) {
    tassert(6183502, "PlanStage cannot be null", root);
    tassert(6142205, "PlanStageData cannot be null", data);
    tassert(6142206, "yieldPolicy cannot be null", yieldPolicy);

    root->attachToOperationContext(opCtx);
    root->attachNewYieldPolicy(yieldPolicy);

    // Call markShouldCollectTimingInfo() if appropriate.
    auto expCtx = cq.getExpCtxRaw();
    tassert(6142207, "No expression context", expCtx);
    if (expCtx->explain || expCtx->mayDbProfile) {
        root->markShouldCollectTimingInfo();
    }

    // Register this plan to yield according to the configured policy.
    yieldPolicy->registerPlan(root);

    root->prepare(data->ctx);

    auto env = data->env;
    // Populate/renew "shardFilterer" if there exists a "shardFilterer" slot. The slot value should
    // be set to Nothing in the plan cache to avoid extending the lifetime of the ownership filter.
    if (auto shardFiltererSlot = env->getSlotIfExists("shardFilterer"_sd)) {
        const auto& collection = collections.getMainCollection();
        tassert(6108307,
                "Setting shard filterer slot on un-sharded collection",
                collection.isSharded());

        auto shardFiltererFactory = std::make_unique<ShardFiltererFactoryImpl>(collection);
        auto shardFilterer = shardFiltererFactory->makeShardFilterer(opCtx);
        env->resetSlot(*shardFiltererSlot,
                       sbe::value::TypeTags::shardFilterer,
                       sbe::value::bitcastFrom<ShardFilterer*>(shardFilterer.release()),
                       true);
    }

    // Refresh "let" variables in the 'RuntimeEnvironment'.
    auto ids = expCtx->variablesParseState.getDefinedVariableIDs();
    for (auto id : ids) {
        // Variables defined in "ExpressionContext" may not always be translated into SBE slots.
        if (auto it = data->variableIdToSlotMap.find(id); it != data->variableIdToSlotMap.end()) {
            auto slotId = it->second;
            auto [tag, val] = makeValue(expCtx->variables.getValue(id));
            env->resetSlot(slotId, tag, val, true);
        }
    }

    for (auto&& [id, name] : Variables::kIdToBuiltinVarName) {
        // This can happen if the query that created the cache entry had no value for a system
        // variable, whereas the current query has a value for the system variable but does not
        // actually make use of it in the query plan.
        if (auto slot = env->getSlotIfExists(name); id != Variables::kRootId &&
            id != Variables::kRemoveId && expCtx->variables.hasValue(id) && slot) {
            auto [tag, val] = makeValue(expCtx->variables.getValue(id));
            env->resetSlot(*slot, tag, val, true);
        }
    }

    input_params::bind(cq, data->inputParamToSlotMap, env, preparingFromCache);

    for (auto&& indexBoundsInfo : data->indexBoundsEvaluationInfos) {
        input_params::bindIndexBounds(cq, indexBoundsInfo, env);
    }
}

PlanStageSlots::PlanStageSlots(const PlanStageReqs& reqs,
                               sbe::value::SlotIdGenerator* slotIdGenerator) {
    for (auto&& [slotName, isRequired] : reqs._slots) {
        if (isRequired) {
            _slots[slotName] = slotIdGenerator->generate();
        }
    }
}

std::string PlanStageData::debugString() const {
    StringBuilder builder;

    if (auto slot = outputs.getIfExists(PlanStageSlots::kResult); slot) {
        builder << "$$RESULT=s" << *slot << " ";
    }
    if (auto slot = outputs.getIfExists(PlanStageSlots::kRecordId); slot) {
        builder << "$$RID=s" << *slot << " ";
    }

    env->debugString(&builder);

    return builder.str();
}

namespace {
void getAllNodesByTypeHelper(const QuerySolutionNode* root,
                             StageType type,
                             std::vector<const QuerySolutionNode*>& results) {
    if (root->getType() == type) {
        results.push_back(root);
    }

    for (auto&& child : root->children) {
        getAllNodesByTypeHelper(child.get(), type, results);
    }
}

std::vector<const QuerySolutionNode*> getAllNodesByType(const QuerySolutionNode* root,
                                                        StageType type) {
    std::vector<const QuerySolutionNode*> results;
    getAllNodesByTypeHelper(root, type, results);
    return results;
}

/**
 * Returns pair consisting of:
 *  - First node of the specified type found by pre-order traversal. If node was not found, this
 *    pair element is nullptr.
 *  - Total number of nodes with the specified type in tree.
 */
std::pair<const QuerySolutionNode*, size_t> getFirstNodeByType(const QuerySolutionNode* root,
                                                               StageType type) {
    const QuerySolutionNode* result = nullptr;
    size_t count = 0;
    if (root->getType() == type) {
        result = root;
        count++;
    }

    for (auto&& child : root->children) {
        auto [subTreeResult, subTreeCount] = getFirstNodeByType(child.get(), type);
        if (!result) {
            result = subTreeResult;
        }
        count += subTreeCount;
    }

    return {result, count};
}

/**
 * Returns node of the specified type found in tree. If there is no such node, returns null. If
 * there are more than one nodes of the specified type, throws an exception.
 */
const QuerySolutionNode* getLoneNodeByType(const QuerySolutionNode* root, StageType type) {
    auto [result, count] = getFirstNodeByType(root, type);
    const auto msgCount = count;
    tassert(5474506,
            str::stream() << "Found " << msgCount << " nodes of type " << stageTypeToString(type)
                          << ", expected one or zero",
            count < 2);
    return result;
}

std::unique_ptr<fts::FTSMatcher> makeFtsMatcher(OperationContext* opCtx,
                                                const CollectionPtr& collection,
                                                const std::string& indexName,
                                                const fts::FTSQuery* ftsQuery) {
    auto desc = collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
    tassert(5432209,
            str::stream() << "index descriptor not found for index named '" << indexName
                          << "' in collection '" << collection->ns() << "'",
            desc);

    auto entry = collection->getIndexCatalog()->getEntry(desc);
    tassert(5432210,
            str::stream() << "index entry not found for index named '" << indexName
                          << "' in collection '" << collection->ns() << "'",
            entry);

    auto accessMethod = static_cast<const FTSAccessMethod*>(entry->accessMethod());
    tassert(5432211,
            str::stream() << "access method is not defined for index named '" << indexName
                          << "' in collection '" << collection->ns() << "'",
            accessMethod);

    // We assume here that node->ftsQuery is an FTSQueryImpl, not an FTSQueryNoop. In practice, this
    // means that it is illegal to use the StageBuilder on a QuerySolution created by planning a
    // query that contains "no-op" expressions.
    auto query = dynamic_cast<const fts::FTSQueryImpl*>(ftsQuery);
    tassert(5432220, "expected FTSQueryImpl", query);
    return std::make_unique<fts::FTSMatcher>(*query, accessMethod->getSpec());
}
}  // namespace

SlotBasedStageBuilder::SlotBasedStageBuilder(OperationContext* opCtx,
                                             const MultipleCollectionAccessor& collections,
                                             const CanonicalQuery& cq,
                                             const QuerySolution& solution,
                                             PlanYieldPolicySBE* yieldPolicy)
    : StageBuilder(opCtx, cq, solution),
      _collections(collections),
      _mainNss(cq.nss()),
      _yieldPolicy(yieldPolicy),
      _data(makeRuntimeEnvironment(_cq, _opCtx, &_slotIdGenerator)),
      _state(_opCtx,
             &_data,
             _cq.getExpCtxRaw()->variables,
             &_slotIdGenerator,
             &_frameIdGenerator,
             &_spoolIdGenerator,
             _cq.getExpCtx()->needsMerge,
             _cq.getExpCtx()->allowDiskUse) {
    // SERVER-52803: In the future if we need to gather more information from the QuerySolutionNode
    // tree, rather than doing one-off scans for each piece of information, we should add a formal
    // analysis pass here.
    // NOTE: Currently, we assume that each query operates on at most one collection, so there can
    // be only one STAGE_COLLSCAN node.
    if (auto node = getLoneNodeByType(solution.root(), STAGE_COLLSCAN)) {
        auto csn = static_cast<const CollectionScanNode*>(node);
        _data.shouldTrackLatestOplogTimestamp = csn->shouldTrackLatestOplogTimestamp;
        _data.shouldTrackResumeToken = csn->requestResumeToken;
        _data.shouldUseTailableScan = csn->tailable;
    }
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::build(const QuerySolutionNode* root) {
    // For a given SlotBasedStageBuilder instance, this build() method can only be called once.
    invariant(!_buildHasStarted);
    _buildHasStarted = true;

    // We always produce a 'resultSlot'.
    PlanStageReqs reqs;
    reqs.set(kResult);
    // We force the root stage to produce a 'recordId' if the iteration can be
    // resumed (via a resume token or a tailable cursor) or if the caller simply expects to be able
    // to read it.
    reqs.setIf(kRecordId,
               (_data.shouldUseTailableScan || _data.shouldTrackResumeToken ||
                _cq.getForceGenerateRecordId()));

    // Set the target namespace to '_mainNss'. This is necessary as some QuerySolutionNodes that
    // require a collection when stage building do not explicitly name which collection they are
    // targeting.
    reqs.setTargetNamespace(_mainNss);

    // Build the SBE plan stage tree.
    auto [stage, outputs] = build(root, reqs);

    // Assert that we produced a 'resultSlot' and that we produced a 'recordIdSlot' only if it was
    // needed.
    invariant(outputs.has(kResult));
    invariant(reqs.has(kRecordId) == outputs.has(kRecordId));

    _data.outputs = std::move(outputs);

    return std::move(stage);
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildCollScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023400, "buildCollScan() does not support kKey", !reqs.hasKeys());

    auto fields = reqs.getFields();
    auto csn = static_cast<const CollectionScanNode*>(root);
    auto [stage, outputs] = generateCollScan(_state,
                                             getCurrentCollection(reqs),
                                             csn,
                                             std::move(fields),
                                             _yieldPolicy,
                                             reqs.getIsTailableCollScanResumeBranch());

    if (reqs.has(kReturnKey)) {
        // Assign the 'returnKeySlot' to be the empty object.
        outputs.set(kReturnKey, _slotIdGenerator.generate());
        stage = sbe::makeProjectStage(
            std::move(stage), root->nodeId(), outputs.get(kReturnKey), makeFunction("newObj"_sd));
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildVirtualScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace std::literals;

    auto vsn = static_cast<const VirtualScanNode*>(root);
    auto reqKeys = reqs.getKeys();

    // The caller should only request kKey slots if the virtual scan is mocking an index scan.
    tassert(6023401,
            "buildVirtualScan() does not support kKey when 'scanType' is not ixscan",
            vsn->scanType == VirtualScanNode::ScanType::kIxscan || reqKeys.empty());

    tassert(6023423,
            "buildVirtualScan() does not support dotted paths for kKey slots",
            std::all_of(reqKeys.begin(), reqKeys.end(), [](auto&& s) {
                return s.find('.') == std::string::npos;
            }));

    auto [inputTag, inputVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard inputGuard{inputTag, inputVal};
    auto inputView = sbe::value::getArrayView(inputVal);

    if (vsn->docs.size()) {
        inputView->reserve(vsn->docs.size());
        for (auto& doc : vsn->docs) {
            auto [tag, val] = makeValue(doc);
            inputView->push_back(tag, val);
        }
    }

    inputGuard.reset();
    auto [scanSlots, scanStage] =
        generateVirtualScanMulti(&_slotIdGenerator, vsn->hasRecordId ? 2 : 1, inputTag, inputVal);

    sbe::value::SlotId resultSlot;
    if (vsn->hasRecordId) {
        invariant(scanSlots.size() == 2);
        resultSlot = scanSlots[1];
    } else {
        invariant(scanSlots.size() == 1);
        resultSlot = scanSlots[0];
    }

    PlanStageSlots outputs;

    if (reqs.has(kResult) || reqs.hasFields()) {
        outputs.set(kResult, resultSlot);
    }
    if (reqs.has(kRecordId)) {
        invariant(vsn->hasRecordId);
        invariant(scanSlots.size() == 2);
        outputs.set(kRecordId, scanSlots[0]);
    }

    auto stage = std::move(scanStage);

    // The caller wants individual slots for certain components of the mock index scan. Retrieve
    // the values for these paths and project them to slots.
    auto [projectStage, slots] = projectTopLevelFields(
        std::move(stage), reqKeys, resultSlot, root->nodeId(), &_slotIdGenerator);
    stage = std::move(projectStage);

    for (size_t i = 0; i < reqKeys.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kKey, std::move(reqKeys[i])), slots[i]);
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildIndexScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto ixn = static_cast<const IndexScanNode*>(root);
    invariant(reqs.has(kReturnKey) || !ixn->addKeyMetadata);

    auto reqKeys = reqs.getKeys();
    auto reqKeysSet = StringDataSet{reqKeys.begin(), reqKeys.end()};

    std::vector<StringData> keys;
    sbe::IndexKeysInclusionSet keysBitset;
    StringDataSet indexKeyPatternSet;
    size_t i = 0;
    for (const auto& elt : ixn->index.keyPattern) {
        StringData name = elt.fieldNameStringData();
        indexKeyPatternSet.emplace(name);
        if (reqKeysSet.count(name)) {
            keysBitset.set(i);
            keys.emplace_back(name);
        }
        ++i;
    }

    for (auto&& key : reqKeys) {
        tassert(7097208,
                str::stream() << "Expected key '" << key << "' to be part of index pattern",
                indexKeyPatternSet.count(key));
    }

    if (reqs.has(kReturnKey) || reqs.has(kResult) || reqs.hasFields()) {
        // If either 'reqs.result' or 'reqs.returnKey' or 'reqs.hasFields()' is true, we need to
        // get all parts of the index key so that we can create the inflated index key.
        for (int j = 0; j < ixn->index.keyPattern.nFields(); ++j) {
            keysBitset.set(j);
        }
    }

    // If the slots necessary for performing an index consistency check were not requested in
    // 'reqs', then don't pass a pointer to 'iamMap' so 'generateIndexScan' doesn't generate the
    // necessary slots.
    auto iamMap = &_data.iamMap;
    if (!(reqs.has(kSnapshotId) && reqs.has(kIndexId) && reqs.has(kIndexKey))) {
        iamMap = nullptr;
    }

    const auto generateIndexScanFunc =
        ixn->iets.empty() ? generateIndexScan : generateIndexScanWithDynamicBounds;
    auto&& [scanStage, scanOutputs] = generateIndexScanFunc(_state,
                                                            getCurrentCollection(reqs),
                                                            ixn,
                                                            keysBitset,
                                                            _yieldPolicy,
                                                            iamMap,
                                                            reqs.has(kIndexKeyPattern));

    auto stage = std::move(scanStage);
    auto outputs = std::move(scanOutputs);

    // Remove the RecordId from the output if we were not requested to produce it.
    if (!reqs.has(PlanStageSlots::kRecordId) && outputs.has(kRecordId)) {
        outputs.clear(kRecordId);
    }

    if (reqs.has(PlanStageSlots::kReturnKey)) {
        sbe::EExpression::Vector args;
        for (auto&& elem : ixn->index.keyPattern) {
            StringData name = elem.fieldNameStringData();
            args.emplace_back(sbe::makeE<sbe::EConstant>(name));
            args.emplace_back(
                makeVariable(outputs.get(std::make_pair(PlanStageSlots::kKey, name))));
        }

        auto rawKeyExpr = sbe::makeE<sbe::EFunction>("newObj"_sd, std::move(args));
        outputs.set(PlanStageSlots::kReturnKey, _slotIdGenerator.generate());
        stage = sbe::makeProjectStage(std::move(stage),
                                      ixn->nodeId(),
                                      outputs.get(PlanStageSlots::kReturnKey),
                                      std::move(rawKeyExpr));
    }

    if (reqs.has(kResult) || reqs.hasFields()) {
        auto indexKeySlots = sbe::makeSV();
        for (auto&& elem : ixn->index.keyPattern) {
            StringData name = elem.fieldNameStringData();
            indexKeySlots.emplace_back(outputs.get(std::make_pair(PlanStageSlots::kKey, name)));
        }

        auto resultSlot = _slotIdGenerator.generate();
        outputs.set(kResult, resultSlot);

        stage = rehydrateIndexKey(
            std::move(stage), ixn->index.keyPattern, ixn->nodeId(), indexKeySlots, resultSlot);
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

namespace {
std::unique_ptr<sbe::EExpression> abtToExpr(optimizer::ABT& abt, optimizer::SlotVarMap& slotMap) {
    auto env = optimizer::VariableEnvironment::build(abt);

    optimizer::PrefixId prefixId;
    // Convert paths into ABT expressions.
    optimizer::EvalPathLowering pathLower{prefixId, env};
    pathLower.optimize(abt);

    // Run the constant folding to eliminate lambda applications as they are not directly
    // supported by the SBE VM.
    optimizer::ConstEval constEval{env};
    constEval.optimize(abt);

    // And finally convert to the SBE expression.
    optimizer::SBEExpressionLowering exprLower{env, slotMap};
    return exprLower.optimize(abt);
}

std::unique_ptr<sbe::EExpression> generatePerColumnPredicate(StageBuilderState& state,
                                                             const MatchExpression* me,
                                                             const sbe::EVariable& inputVar) {
    switch (me->matchType()) {
        // These are always safe since they will never match documents missing their field, or where
        // the element is an object or array.
        case MatchExpression::REGEX:
            return generateRegexExpr(state, checked_cast<const RegexMatchExpression*>(me), inputVar)
                .extractExpr();
        case MatchExpression::MOD:
            return generateModExpr(state, checked_cast<const ModMatchExpression*>(me), inputVar)
                .extractExpr();
        case MatchExpression::BITS_ALL_SET:
            return generateBitTestExpr(state,
                                       checked_cast<const BitTestMatchExpression*>(me),
                                       sbe::BitTestBehavior::AllSet,
                                       inputVar)
                .extractExpr();
        case MatchExpression::BITS_ALL_CLEAR:
            return generateBitTestExpr(state,
                                       checked_cast<const BitTestMatchExpression*>(me),
                                       sbe::BitTestBehavior::AllClear,
                                       inputVar)
                .extractExpr();
        case MatchExpression::BITS_ANY_SET:
            return generateBitTestExpr(state,
                                       checked_cast<const BitTestMatchExpression*>(me),
                                       sbe::BitTestBehavior::AnySet,
                                       inputVar)
                .extractExpr();
        case MatchExpression::BITS_ANY_CLEAR:
            return generateBitTestExpr(state,
                                       checked_cast<const BitTestMatchExpression*>(me),
                                       sbe::BitTestBehavior::AnyClear,
                                       inputVar)
                .extractExpr();
        case MatchExpression::EXISTS:
            return makeConstant(sbe::value::TypeTags::Boolean, true);
        case MatchExpression::LT:
            return generateComparisonExpr(state,
                                          checked_cast<const ComparisonMatchExpression*>(me),
                                          sbe::EPrimBinary::less,
                                          inputVar)
                .extractExpr();
        case MatchExpression::GT:
            return generateComparisonExpr(state,
                                          checked_cast<const ComparisonMatchExpression*>(me),
                                          sbe::EPrimBinary::greater,
                                          inputVar)
                .extractExpr();
        case MatchExpression::EQ:
            return generateComparisonExpr(state,
                                          checked_cast<const ComparisonMatchExpression*>(me),
                                          sbe::EPrimBinary::eq,
                                          inputVar)
                .extractExpr();
        case MatchExpression::LTE:
            return generateComparisonExpr(state,
                                          checked_cast<const ComparisonMatchExpression*>(me),
                                          sbe::EPrimBinary::lessEq,
                                          inputVar)
                .extractExpr();
        case MatchExpression::GTE:
            return generateComparisonExpr(state,
                                          checked_cast<const ComparisonMatchExpression*>(me),
                                          sbe::EPrimBinary::greaterEq,
                                          inputVar)
                .extractExpr();
        case MatchExpression::MATCH_IN: {
            auto expr = checked_cast<const InMatchExpression*>(me);
            tassert(6988583,
                    "Push-down of non-scalar values in $in is not supported.",
                    !expr->hasNonScalarOrNonEmptyValues());
            return generateInExpr(state, expr, inputVar).extractExpr();
        }
        case MatchExpression::TYPE_OPERATOR: {
            const auto& expr = checked_cast<const TypeMatchExpression*>(me);
            const MatcherTypeSet& ts = expr->typeSet();

            return makeFunction(
                "typeMatch",
                inputVar.clone(),
                makeConstant(sbe::value::TypeTags::NumberInt64,
                             sbe::value::bitcastFrom<int64_t>(ts.getBSONTypeMask())));
        }
        case MatchExpression::NOT: {
            uasserted(6733604, "(TODO SERVER-69610) need expr translation to enable $not");
        }

        default:
            uasserted(6733605,
                      std::string("Expression ") + me->serialize().toString() +
                          " should not be pushed down as a per-column filter");
    }
    MONGO_UNREACHABLE;
}

std::unique_ptr<sbe::EExpression> generateLeafExpr(StageBuilderState& state,
                                                   const MatchExpression* me,
                                                   sbe::FrameId lambdaFrameId,
                                                   sbe::value::SlotId inputSlot) {
    sbe::EVariable lambdaParam = sbe::EVariable{lambdaFrameId, 0};

    auto lambdaExpr = sbe::makeE<sbe::ELocalLambda>(
        lambdaFrameId, generatePerColumnPredicate(state, me, lambdaParam));

    const MatchExpression::MatchType mt = me->matchType();
    auto traverserName = (mt == MatchExpression::EXISTS || mt == MatchExpression::TYPE_OPERATOR)
        ? "traverseCsiCellTypes"
        : "traverseCsiCellValues";
    return makeFunction(traverserName, makeVariable(inputSlot), std::move(lambdaExpr));
}

std::unique_ptr<sbe::EExpression> generatePerColumnLogicalAndExpr(StageBuilderState& state,
                                                                  const AndMatchExpression* me,
                                                                  sbe::FrameId lambdaFrameId,
                                                                  sbe::value::SlotId inputSlot) {
    const auto cTerms = me->numChildren();
    tassert(7072600, "AND should have at least one child", cTerms > 0);

    std::vector<std::unique_ptr<sbe::EExpression>> leaves;
    leaves.reserve(cTerms);
    for (size_t i = 0; i < cTerms; i++) {
        leaves.push_back(generateLeafExpr(state, me->getChild(i), lambdaFrameId, inputSlot));
    }

    // Create the balanced binary tree to keep the tree shallow and safe for recursion.
    return makeBalancedBooleanOpTree(sbe::EPrimBinary::logicAnd, std::move(leaves));
}

std::unique_ptr<sbe::EExpression> generatePerColumnFilterExpr(StageBuilderState& state,
                                                              const MatchExpression* me,
                                                              sbe::value::SlotId inputSlot) {
    auto lambdaFrameId = state.frameIdGenerator->generate();

    if (me->matchType() == MatchExpression::AND) {
        return generatePerColumnLogicalAndExpr(
            state, checked_cast<const AndMatchExpression*>(me), lambdaFrameId, inputSlot);
    }

    return generateLeafExpr(state, me, lambdaFrameId, inputSlot);
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildColumnScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023403, "buildColumnScan() does not support kKey", !reqs.hasKeys());

    auto csn = static_cast<const ColumnIndexScanNode*>(root);
    tassert(6312405,
            "Unexpected filter provided for column scan stage. Expected 'filtersByPath' or "
            "'postAssemblyFilter' to be used instead.",
            !csn->filter);

    PlanStageSlots outputs;

    auto reconstructedRecordSlot = _slotIdGenerator.generate();
    outputs.set(kResult, reconstructedRecordSlot);

    boost::optional<sbe::value::SlotId> ridSlot;

    if (reqs.has(kRecordId)) {
        ridSlot = _slotIdGenerator.generate();
        outputs.set(kRecordId, *ridSlot);
    }

    auto fieldSlotIds = _slotIdGenerator.generateMultiple(csn->allFields.size());
    auto rowStoreSlot = _slotIdGenerator.generate();

    // Get all the paths but make sure "_id" comes first (the order of paths given to the
    // column_scan stage defines the order of fields in the reconstructed record).
    std::vector<std::string> paths;
    paths.reserve(csn->allFields.size());
    if (csn->allFields.find("_id") != csn->allFields.end()) {
        paths.push_back("_id");
    }
    for (const auto& path : csn->allFields) {
        if (path != "_id") {
            paths.push_back(path);
        }
    }

    // Identify the filtered columns, if any, and create slots/expressions for them.
    std::vector<sbe::ColumnScanStage::PathFilter> filteredPaths;
    filteredPaths.reserve(csn->filtersByPath.size());
    for (size_t i = 0; i < paths.size(); i++) {
        auto itFilter = csn->filtersByPath.find(paths[i]);
        if (itFilter != csn->filtersByPath.end()) {
            auto filterInputSlot = _slotIdGenerator.generate();

            filteredPaths.emplace_back(
                i,
                generatePerColumnFilterExpr(_state, itFilter->second.get(), filterInputSlot),
                filterInputSlot);
        }
    }

    // Tag which of the paths should be included into the output.
    DepsTracker residual;
    if (csn->postAssemblyFilter) {
        match_expression::addDependencies(csn->postAssemblyFilter.get(), &residual);
    }
    std::vector<bool> includeInOutput(paths.size(), false);
    OrderedPathSet fieldsToProject;  // projection when falling back to the row store
    for (size_t i = 0; i < paths.size(); i++) {
        if (csn->outputFields.find(paths[i]) != csn->outputFields.end() ||
            residual.fields.find(paths[i]) != residual.fields.end()) {
            includeInOutput[i] = true;
            fieldsToProject.insert(paths[i]);
        }
    }

    const optimizer::ProjectionName rootStr = "rowStoreRoot";
    optimizer::FieldMapBuilder builder(rootStr, true);

    // When building its output document (in 'recordSlot'), the 'ColumnStoreStage' should not try to
    // separately project both a document and its sub-fields (e.g., both 'a' and 'a.b'). Compute the
    // the subset of 'csn->allFields' that only includes a field if no other field in
    // 'csn->allFields' is its prefix.
    fieldsToProject =
        DepsTracker::simplifyDependencies(fieldsToProject, DepsTracker::TruncateToRootLevel::no);
    for (const std::string& field : fieldsToProject) {
        builder.integrateFieldPath(FieldPath(field),
                                   [](const bool isLastElement, optimizer::FieldMapEntry& entry) {
                                       entry._hasLeadingObj = true;
                                       entry._hasKeep = true;
                                   });
    }

    // Generate the expression that is applied to the row store record (in the case when the result
    // cannot be reconstructed from the index).
    std::unique_ptr<sbe::EExpression> rowStoreExpr = nullptr;

    // Avoid generating the row store expression if the projection is not necessary, as indicated by
    // the extraFieldsPermitted flag of the column store node.
    if (boost::optional<optimizer::ABT> abt;
        !csn->extraFieldsPermitted && (abt = builder.generateABT())) {
        // We might get null abt if no paths were added to the builder. It means we should be
        // projecting an empty object.
        tassert(
            6935000, "ABT must be valid if have fields to project", fieldsToProject.empty() || abt);
        optimizer::SlotVarMap slotMap{};
        slotMap[rootStr] = rowStoreSlot;
        rowStoreExpr = abt ? abtToExpr(*abt, slotMap)
                           : sbe::makeE<sbe::EFunction>("newObj", sbe::EExpression::Vector{});
    }

    std::unique_ptr<sbe::PlanStage> stage =
        std::make_unique<sbe::ColumnScanStage>(getCurrentCollection(reqs)->uuid(),
                                               csn->indexEntry.identifier.catalogName,
                                               std::move(paths),
                                               std::move(includeInOutput),
                                               ridSlot,
                                               reconstructedRecordSlot,
                                               rowStoreSlot,
                                               std::move(rowStoreExpr),
                                               std::move(filteredPaths),
                                               _yieldPolicy,
                                               csn->nodeId());

    // Generate post assembly filter.
    if (csn->postAssemblyFilter) {
        auto relevantSlots = sbe::makeSV(reconstructedRecordSlot);
        if (ridSlot) {
            relevantSlots.push_back(*ridSlot);
        }

        auto [_, outputStage] = generateFilter(_state,
                                               csn->postAssemblyFilter.get(),
                                               {std::move(stage), std::move(relevantSlots)},
                                               reconstructedRecordSlot,
                                               &outputs,
                                               csn->nodeId());
        stage = outputStage.extractStage(csn->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildFetch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto fn = static_cast<const FetchNode*>(root);

    // The child must produce a kRecordId slot, as well as all the kMeta and kKey slots required
    // by the parent of this FetchNode except for 'resultSlot'. Note that the child does _not_
    // need to produce any kField slots. Any kField requests by the parent will be handled by the
    // logic below.
    auto child = fn->children[0].get();

    auto forwardingReqs = reqs.copy().clear(kResult).clear(kRecordId).clearAllFields();

    auto childReqs = forwardingReqs.copy()
                         .set(kRecordId)
                         .set(kSnapshotId)
                         .set(kIndexId)
                         .set(kIndexKey)
                         .set(kIndexKeyPattern);

    auto [stage, outputs] = build(child, childReqs);

    auto iamMap = _data.iamMap;

    uassert(4822880, "RecordId slot is not defined", outputs.has(kRecordId));
    uassert(
        4953600, "ReturnKey slot is not defined", !reqs.has(kReturnKey) || outputs.has(kReturnKey));
    uassert(5290701, "Snapshot id slot is not defined", outputs.has(kSnapshotId));
    uassert(5290702, "Index id slot is not defined", outputs.has(kIndexId));
    uassert(5290711, "Index key slot is not defined", outputs.has(kIndexKey));
    uassert(5113713, "Index key pattern slot is not defined", outputs.has(kIndexKeyPattern));

    auto fields = reqs.getFields();

    if (fn->filter) {
        DepsTracker deps;
        match_expression::addDependencies(fn->filter.get(), &deps);
        // If the filter predicate doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!deps.needWholeDocument) {
            auto topLevelFields = getTopLevelFields(deps.fields);
            fields = appendVectorUnique(std::move(fields), std::move(topLevelFields));
        }
    }

    auto childRecordId = outputs.get(kRecordId);
    auto fetchResultSlot = _slotIdGenerator.generate();
    auto fetchRecordIdSlot = _slotIdGenerator.generate();
    auto fieldSlots = _slotIdGenerator.generateMultiple(fields.size());

    auto relevantSlots = getSlotsToForward(forwardingReqs, outputs);

    stage = makeLoopJoinForFetch(std::move(stage),
                                 fetchResultSlot,
                                 fetchRecordIdSlot,
                                 fields,
                                 fieldSlots,
                                 childRecordId,
                                 outputs.get(kSnapshotId),
                                 outputs.get(kIndexId),
                                 outputs.get(kIndexKey),
                                 outputs.get(kIndexKeyPattern),
                                 getCurrentCollection(reqs),
                                 std::move(iamMap),
                                 root->nodeId(),
                                 std::move(relevantSlots));

    outputs.set(kResult, fetchResultSlot);

    // Only propagate kRecordId if requested.
    if (reqs.has(kRecordId)) {
        outputs.set(kRecordId, fetchRecordIdSlot);
    } else {
        outputs.clear(kRecordId);
    }

    for (size_t i = 0; i < fields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, fields[i]), fieldSlots[i]);
    }

    if (fn->filter) {
        auto forwardingReqs = reqs.copy().set(kResult).setFields(fields);
        auto relevantSlots = getSlotsToForward(forwardingReqs, outputs);

        auto [_, outputStage] = generateFilter(_state,
                                               fn->filter.get(),
                                               {std::move(stage), std::move(relevantSlots)},
                                               outputs.get(kResult),
                                               &outputs,
                                               root->nodeId());
        stage = outputStage.extractStage(root->nodeId());
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildLimit(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto ln = static_cast<const LimitNode*>(root);
    boost::optional<long long> skip;

    auto [stage, outputs] = [&]() {
        if (ln->children[0]->getType() == StageType::STAGE_SKIP) {
            // If we have both limit and skip stages and the skip stage is beneath the limit, then
            // we can combine these two stages into one.
            const auto sn = static_cast<const SkipNode*>(ln->children[0].get());
            skip = sn->skip;
            return build(sn->children[0].get(), reqs);
        } else {
            return build(ln->children[0].get(), reqs);
        }
    }();

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage), ln->limit, skip, root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSkip(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto sn = static_cast<const SkipNode*>(root);
    auto [stage, outputs] = build(sn->children[0].get(), reqs);

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage), boost::none, sn->skip, root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

namespace {
/**
 * Given a field path, this function will return an expression that will be true if evaluating the
 * field path involves array traversal at any level of the path (including the leaf field).
 */
std::unique_ptr<sbe::EExpression> generateArrayCheckForSort(
    std::unique_ptr<sbe::EExpression> inputExpr,
    const FieldPath& fp,
    FieldIndex level,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    boost::optional<sbe::value::SlotId> fieldSlot = boost::none) {
    invariant(level < fp.getPathLength());

    auto fieldExpr = fieldSlot
        ? makeVariable(*fieldSlot)
        : makeFunction("getField"_sd, std::move(inputExpr), makeConstant(fp.getFieldName(level)));

    auto resultExpr = [&] {
        if (level == fp.getPathLength() - 1u) {
            return makeFunction("isArray"_sd, std::move(fieldExpr));
        }
        auto frameId = fieldSlot ? boost::optional<sbe::FrameId>{}
                                 : boost::make_optional(frameIdGenerator->generate());
        auto var = fieldSlot ? std::move(fieldExpr) : makeVariable(*frameId, 0);
        auto resultExpr =
            makeBinaryOp(sbe::EPrimBinary::logicOr,
                         makeFunction("isArray"_sd, var->clone()),
                         generateArrayCheckForSort(var->clone(), fp, level + 1, frameIdGenerator));

        if (!fieldSlot) {
            resultExpr = sbe::makeE<sbe::ELocalBind>(
                *frameId, sbe::makeEs(std::move(fieldExpr)), std::move(resultExpr));
        }
        return resultExpr;
    }();

    if (level == 0) {
        resultExpr = makeFillEmptyFalse(std::move(resultExpr));
    }

    return resultExpr;
}

/**
 * Given a field path, this function recursively builds an expression tree that will produce the
 * corresponding sort key for that path.
 */
std::unique_ptr<sbe::EExpression> generateSortTraverse(
    const sbe::EVariable& inputVar,
    bool isAscending,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const FieldPath& fp,
    size_t level,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    boost::optional<sbe::value::SlotId> fieldSlot = boost::none) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());

    StringData helperFn = isAscending ? "_internalLeast"_sd : "_internalGreatest"_sd;

    // Generate an expression to read a sub-field at the current nested level.
    auto fieldExpr = fieldSlot
        ? makeVariable(*fieldSlot)
        : makeFunction("getField"_sd, inputVar.clone(), makeConstant(fp.getFieldName(level)));

    if (level == fp.getPathLength() - 1) {
        // For the last level, we can just return the field slot without the need for a
        // traverse expression.
        auto frameId = fieldSlot ? boost::optional<sbe::FrameId>{}
                                 : boost::make_optional(frameIdGenerator->generate());
        auto var = fieldSlot ? fieldExpr->clone() : makeVariable(*frameId, 0);
        auto moveVar = fieldSlot ? std::move(fieldExpr) : makeMoveVariable(*frameId, 0);

        auto helperArgs = sbe::makeEs(moveVar->clone());
        if (collatorSlot) {
            helperArgs.emplace_back(makeVariable(*collatorSlot));
        }

        // According to MQL's sorting semantics, when a leaf field is an empty array we
        // should use Undefined as the sort key.
        auto resultExpr = sbe::makeE<sbe::EIf>(
            makeFillEmptyFalse(makeFunction("isArray"_sd, std::move(var))),
            makeFillEmptyUndefined(sbe::makeE<sbe::EFunction>(helperFn, std::move(helperArgs))),
            makeFillEmptyNull(std::move(moveVar)));

        if (!fieldSlot) {
            resultExpr = sbe::makeE<sbe::ELocalBind>(
                *frameId, sbe::makeEs(std::move(fieldExpr)), std::move(resultExpr));
        }
        return resultExpr;
    }

    // Prepare a lambda expression that will navigate to the next component of the field path.
    auto lambdaFrameId = frameIdGenerator->generate();
    auto lambdaExpr =
        sbe::makeE<sbe::ELocalLambda>(lambdaFrameId,
                                      generateSortTraverse(sbe::EVariable{lambdaFrameId, 0},
                                                           isAscending,
                                                           collatorSlot,
                                                           fp,
                                                           level + 1,
                                                           frameIdGenerator));

    // Generate the traverse expression for the current nested level.
    // Be sure to invoke the least/greatest fold expression only if the current nested level is an
    // array.
    auto frameId = frameIdGenerator->generate();
    auto var = fieldSlot ? makeVariable(*fieldSlot) : makeVariable(frameId, 0);
    auto resultVar = makeMoveVariable(frameId, fieldSlot ? 0 : 1);

    auto binds = sbe::makeEs();
    if (!fieldSlot) {
        binds.emplace_back(std::move(fieldExpr));
    }
    binds.emplace_back(
        makeFunction("traverseP",
                     var->clone(),
                     std::move(lambdaExpr),
                     makeConstant(sbe::value::TypeTags::NumberInt32, 1) /* maxDepth */));

    auto helperArgs = sbe::makeEs(resultVar->clone());
    if (collatorSlot) {
        helperArgs.emplace_back(makeVariable(*collatorSlot));
    }

    return sbe::makeE<sbe::ELocalBind>(
        frameId,
        std::move(binds),
        // According to MQL's sorting semantics, when a non-leaf field is an empty array or
        // doesn't exist we should use Null as the sort key.
        makeFillEmptyNull(
            sbe::makeE<sbe::EIf>(makeFillEmptyFalse(makeFunction("isArray"_sd, var->clone())),
                                 sbe::makeE<sbe::EFunction>(helperFn, std::move(helperArgs)),
                                 resultVar->clone())));
}

void visitPatternTreeLeaves(
    IndexKeyPatternTreeNode* patternRoot,
    const std::function<void(const std::string&, IndexKeyPatternTreeNode*)>& fn) {
    tassert(7097209,
            "Expected non-empty pattern",
            patternRoot && patternRoot->childrenOrder.size() >= 1);

    // Perform a depth-first traversal using 'visitTreeStack' to keep track of where we are
    std::vector<std::pair<IndexKeyPatternTreeNode*, size_t>> visitTreeStack;
    std::string path;
    visitTreeStack.emplace_back(patternRoot, 0);
    while (!visitTreeStack.empty()) {
        auto [node, idx] = visitTreeStack.back();
        if (idx < node->childrenOrder.size()) {
            const auto& childName = node->childrenOrder[idx];
            visitTreeStack.back().second = idx + 1;
            visitTreeStack.emplace_back(node->children[childName].get(), 0);
            if (!path.empty()) {
                path.append(1, '.');
            }
            path += childName;
        } else {
            // If this is a leaf node, invoke the callback
            if (node->childrenOrder.empty()) {
                fn(path, node);
            }
            visitTreeStack.pop_back();
            auto pos = path.find_last_of('.');
            path.resize(pos != std::string::npos ? pos : 0);
        }
    }
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSort(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto sn = static_cast<const SortNode*>(root);
    auto sortPattern = SortPattern{sn->pattern, _cq.getExpCtx()};

    tassert(5037001,
            "QueryPlannerAnalysis should not produce a SortNode with an empty sort pattern",
            sortPattern.size() > 0);

    auto child = sn->children[0].get();

    if (auto [ixn, ct] = getFirstNodeByType(root, STAGE_IXSCAN);
        !sn->fetched() && !reqs.has(kResult) && ixn && ct == 1) {
        return buildSortCovered(root, reqs);
    }

    StringDataSet prefixSet;
    bool hasPartsWithCommonPrefix = false;
    for (const auto& part : sortPattern) {
        // getExecutor() should never call into buildSlotBasedExecutableTree() when the query
        // contains $meta, so this assertion should always be true.
        tassert(5037002, "Sort with $meta is not supported in SBE", part.fieldPath);

        if (!hasPartsWithCommonPrefix) {
            auto [_, prefixWasNotPresent] = prefixSet.insert(part.fieldPath->getFieldName(0));
            hasPartsWithCommonPrefix = !prefixWasNotPresent;
        }
    }

    auto fields = reqs.getFields();

    if (!hasPartsWithCommonPrefix) {
        DepsTracker deps;
        sortPattern.addDependencies(&deps);
        // If the sort pattern doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!deps.needWholeDocument) {
            auto topLevelFields = getTopLevelFields(deps.fields);
            fields = appendVectorUnique(std::move(fields), std::move(topLevelFields));
        }
    }

    auto childReqs = reqs.copy().set(kResult).setFields(fields);
    auto [stage, childOutputs] = build(child, childReqs);
    auto outputs = std::move(childOutputs);

    auto collatorSlot = _data.env->getSlotIfExists("collator"_sd);

    sbe::value::SlotVector orderBy;
    std::vector<sbe::value::SortDirection> direction;
    sbe::value::SlotId outputSlotId = outputs.get(kResult);

    if (!hasPartsWithCommonPrefix) {
        // Handle the case where we are using kResult and there are no common prefixes.
        orderBy.reserve(sortPattern.size());

        // Sorting has a limitation where only one of the sort patterns can involve arrays.
        // If there are at least two sort patterns, check the data for this possibility.
        auto failOnParallelArrays = [&]() -> std::unique_ptr<mongo::sbe::EExpression> {
            auto parallelArraysError = sbe::makeE<sbe::EFail>(
                ErrorCodes::BadValue, "cannot sort with keys that are parallel arrays");

            if (sortPattern.size() < 2) {
                // If the sort pattern only has one part, we don't need to generate a "parallel
                // arrays" check.
                return {};
            } else if (sortPattern.size() == 2) {
                // If the sort pattern has two parts, we can generate a simpler expression to
                // perform the "parallel arrays" check.
                auto makeIsNotArrayCheck = [&](const FieldPath& fp) {
                    return makeNot(generateArrayCheckForSort(
                        makeVariable(outputSlotId),
                        fp,
                        0 /* level */,
                        &_frameIdGenerator,
                        outputs.getIfExists(
                            std::make_pair(PlanStageSlots::kField, fp.getFieldName(0)))));
                };

                return makeBinaryOp(sbe::EPrimBinary::logicOr,
                                    makeIsNotArrayCheck(*sortPattern[0].fieldPath),
                                    makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                 makeIsNotArrayCheck(*sortPattern[1].fieldPath),
                                                 std::move(parallelArraysError)));
            } else {
                // If the sort pattern has three or more parts, we generate an expression to
                // perform the "parallel arrays" check that works (and scales well) for an
                // arbitrary number of sort pattern parts.
                auto makeIsArrayCheck = [&](const FieldPath& fp) {
                    return makeBinaryOp(
                        sbe::EPrimBinary::cmp3w,
                        generateArrayCheckForSort(makeVariable(outputSlotId),
                                                  fp,
                                                  0,
                                                  &_frameIdGenerator,
                                                  outputs.getIfExists(std::make_pair(
                                                      PlanStageSlots::kField, fp.getFieldName(0)))),
                        makeConstant(sbe::value::TypeTags::Boolean, false));
                };

                auto numArraysExpr = makeIsArrayCheck(*sortPattern[0].fieldPath);
                for (size_t idx = 1; idx < sortPattern.size(); ++idx) {
                    numArraysExpr = makeBinaryOp(sbe::EPrimBinary::add,
                                                 std::move(numArraysExpr),
                                                 makeIsArrayCheck(*sortPattern[idx].fieldPath));
                }

                return makeBinaryOp(
                    sbe::EPrimBinary::logicOr,
                    makeBinaryOp(sbe::EPrimBinary::lessEq,
                                 std::move(numArraysExpr),
                                 makeConstant(sbe::value::TypeTags::NumberInt32, 1)),
                    std::move(parallelArraysError));
            }
        }();

        if (failOnParallelArrays) {
            stage = sbe::makeProjectStage(std::move(stage),
                                          root->nodeId(),
                                          _slotIdGenerator.generate(),
                                          std::move(failOnParallelArrays));
        }

        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> sortExpressions;

        for (const auto& part : sortPattern) {
            auto topLevelFieldSlot = outputs.get(
                std::make_pair(PlanStageSlots::kField, part.fieldPath->getFieldName(0)));

            std::unique_ptr<sbe::EExpression> sortExpr =
                generateSortTraverse(sbe::EVariable{outputSlotId},
                                     part.isAscending,
                                     collatorSlot,
                                     *part.fieldPath,
                                     0,
                                     &_frameIdGenerator,
                                     topLevelFieldSlot);

            // Apply the transformation required by the collation, if specified.
            if (collatorSlot) {
                sortExpr = makeFunction(
                    "collComparisonKey"_sd, std::move(sortExpr), makeVariable(*collatorSlot));
            }
            sbe::value::SlotId sortKeySlot = _slotIdGenerator.generate();
            sortExpressions.emplace(sortKeySlot, std::move(sortExpr));

            orderBy.push_back(sortKeySlot);
            direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                                 : sbe::value::SortDirection::Descending);
        }
        stage = sbe::makeS<sbe::ProjectStage>(
            std::move(stage), std::move(sortExpressions), root->nodeId());

    } else {
        // Handle the case where two or more parts of the sort pattern have a common prefix.
        orderBy = _slotIdGenerator.generateMultiple(1);
        direction = {sbe::value::SortDirection::Ascending};

        auto sortSpec = std::make_unique<sbe::value::SortSpec>(sn->pattern);
        auto sortSpecExpr =
            makeConstant(sbe::value::TypeTags::sortSpec,
                         sbe::value::bitcastFrom<sbe::value::SortSpec*>(sortSpec.release()));

        // generateSortKey() will handle the parallel arrays check and sort key traversal for us,
        // so we don't need to generate our own sort key traversal logic in the SBE plan.
        stage = sbe::makeProjectStage(std::move(stage),
                                      root->nodeId(),
                                      orderBy[0],
                                      collatorSlot ? makeFunction("generateSortKey",
                                                                  std::move(sortSpecExpr),
                                                                  makeVariable(outputSlotId),
                                                                  makeVariable(*collatorSlot))
                                                   : makeFunction("generateSortKey",
                                                                  std::move(sortSpecExpr),
                                                                  makeVariable(outputSlotId)));
    }

    // Slots for sort stage to forward to parent stage. Values in these slots are not used during
    // sorting.
    auto forwardedSlots = getSlotsToForward(reqs, outputs);

    stage =
        sbe::makeS<sbe::SortStage>(std::move(stage),
                                   std::move(orderBy),
                                   std::move(direction),
                                   std::move(forwardedSlots),
                                   sn->limit ? sn->limit : std::numeric_limits<std::size_t>::max(),
                                   sn->maxMemoryUsageBytes,
                                   _cq.getExpCtx()->allowDiskUse,
                                   root->nodeId());

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSortCovered(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023404, "buildSortCovered() does not support kResult", !reqs.has(kResult));

    const auto sn = static_cast<const SortNode*>(root);
    auto sortPattern = SortPattern{sn->pattern, _cq.getExpCtx()};

    tassert(7047600,
            "QueryPlannerAnalysis should not produce a SortNode with an empty sort pattern",
            sortPattern.size() > 0);
    tassert(6023422, "buildSortCovered() expected 'sn' to not be fetched", !sn->fetched());

    auto child = sn->children[0].get();
    auto indexScan = static_cast<const IndexScanNode*>(getLoneNodeByType(child, STAGE_IXSCAN));
    tassert(7047601, "Expected index scan below sort", indexScan);
    auto indexKeyPattern = indexScan->index.keyPattern;

    // The child must produce all of the slots required by the parent of this SortNode.
    auto childReqs = reqs.copy();

    std::vector<std::string> keys;
    StringDataSet sortPathsSet;
    for (const auto& part : sortPattern) {
        const auto& key = part.fieldPath->fullPath();
        keys.emplace_back(key);
        sortPathsSet.emplace(key);
    }

    childReqs.setKeys(std::move(keys));

    auto [stage, outputs] = build(child, childReqs);

    auto collatorSlot = _data.env->getSlotIfExists("collator"_sd);

    sbe::value::SlotVector orderBy;
    std::vector<sbe::value::SortDirection> direction;
    orderBy.reserve(sortPattern.size());
    direction.reserve(sortPattern.size());
    for (const auto& part : sortPattern) {
        // getExecutor() should never call into buildSlotBasedExecutableTree() when the query
        // contains $meta, so this assertion should always be true.
        tassert(7047602, "Sort with $meta is not supported in SBE", part.fieldPath);

        orderBy.push_back(
            outputs.get(std::make_pair(PlanStageSlots::kKey, part.fieldPath->fullPath())));
        direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                             : sbe::value::SortDirection::Descending);
    }

    // If a collation is set, generate a ProjectStage that calls collComparisonKey() on each
    // field in the sort pattern. The "comparison keys" returned by collComparisonKey() will
    // be used in 'orderBy' instead of the fields' actual values.
    if (collatorSlot) {
        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projectMap;
        auto makeSortKey = [&](sbe::value::SlotId inputSlot) {
            return makeFunction(
                "collComparisonKey"_sd, makeVariable(inputSlot), makeVariable(*collatorSlot));
        };

        for (size_t idx = 0; idx < orderBy.size(); ++idx) {
            auto sortKeySlot{_slotIdGenerator.generate()};
            projectMap.emplace(sortKeySlot, makeSortKey(orderBy[idx]));
            orderBy[idx] = sortKeySlot;
        }

        stage =
            sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projectMap), root->nodeId());
    }

    // Slots for sort stage to forward to parent stage. Values in these slots are not used during
    // sorting.
    auto forwardedSlots = getSlotsToForward(childReqs, outputs, orderBy);

    stage =
        sbe::makeS<sbe::SortStage>(std::move(stage),
                                   std::move(orderBy),
                                   std::move(direction),
                                   std::move(forwardedSlots),
                                   sn->limit ? sn->limit : std::numeric_limits<std::size_t>::max(),
                                   sn->maxMemoryUsageBytes,
                                   _cq.getExpCtx()->allowDiskUse,
                                   root->nodeId());

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildSortKeyGenerator(const QuerySolutionNode* root,
                                             const PlanStageReqs& reqs) {
    uasserted(4822883, "Sort key generator in not supported in SBE yet");
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSortMerge(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace std::literals;
    auto mergeSortNode = static_cast<const MergeSortNode*>(root);

    const auto sortPattern = SortPattern{mergeSortNode->sort, _cq.getExpCtx()};
    std::vector<sbe::value::SortDirection> direction;

    for (const auto& part : sortPattern) {
        uassert(4822881, "Sorting by expression not supported", !part.expression);
        direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                             : sbe::value::SortDirection::Descending);
    }

    sbe::PlanStage::Vector inputStages;
    std::vector<sbe::value::SlotVector> inputKeys;
    std::vector<sbe::value::SlotVector> inputVals;

    std::vector<std::string> keys;
    StringSet sortPatternSet;
    for (auto&& sortPart : sortPattern) {
        sortPatternSet.emplace(sortPart.fieldPath->fullPath());
        keys.emplace_back(sortPart.fieldPath->fullPath());
    }

    // Children must produce all of the slots required by the parent of this SortMergeNode. In
    // addition, children must always produce a 'recordIdSlot' if the 'dedup' flag is true, and
    // they must produce kKey slots for each part of the sort pattern.
    auto childReqs = reqs.copy().setIf(kRecordId, mergeSortNode->dedup).setKeys(std::move(keys));

    for (auto&& child : mergeSortNode->children) {
        sbe::value::SlotVector inputKeysForChild;

        // Children must produce a 'resultSlot' if they produce fetched results.
        auto [stage, outputs] = build(child.get(), childReqs);

        tassert(5184301,
                "SORT_MERGE node must receive a RecordID slot as input from child stage"
                " if the 'dedup' flag is set",
                !mergeSortNode->dedup || outputs.has(kRecordId));

        for (const auto& part : sortPattern) {
            inputKeysForChild.push_back(
                outputs.get(std::make_pair(PlanStageSlots::kKey, part.fieldPath->fullPath())));
        }

        inputKeys.push_back(std::move(inputKeysForChild));
        inputStages.push_back(std::move(stage));

        auto sv = getSlotsToForward(childReqs, outputs);

        inputVals.push_back(std::move(sv));
    }

    PlanStageSlots outputs(childReqs, &_slotIdGenerator);

    auto outputVals = getSlotsToForward(childReqs, outputs);

    auto stage = sbe::makeS<sbe::SortedMergeStage>(std::move(inputStages),
                                                   std::move(inputKeys),
                                                   std::move(direction),
                                                   std::move(inputVals),
                                                   std::move(outputVals),
                                                   root->nodeId());

    if (mergeSortNode->dedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(kRecordId)), root->nodeId());
        // Stop propagating the RecordId output if none of our ancestors are going to use it.
        if (!reqs.has(kRecordId)) {
            outputs.clear(kRecordId);
        }
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionSimple(const QuerySolutionNode* root,
                                             const PlanStageReqs& reqs) {
    using namespace std::literals;
    tassert(6023405, "buildProjectionSimple() does not support kKey", !reqs.hasKeys());

    auto pn = static_cast<const ProjectionNodeSimple*>(root);

    auto [fields, additionalFields] = splitVector(reqs.getFields(), [&](const std::string& s) {
        return pn->proj.type() == projection_ast::ProjectType::kInclusion
            ? pn->proj.getRequiredFields().count(s)
            : !pn->proj.getExcludedPaths().count(s);
    });

    auto childReqs = reqs.copy().clearAllFields().setFields(std::move(fields));
    auto [stage, childOutputs] = build(pn->children[0].get(), childReqs);
    auto outputs = std::move(childOutputs);

    if (reqs.has(kResult)) {
        const auto childResult = outputs.get(kResult);

        sbe::MakeBsonObjStage::FieldBehavior behaviour;
        const OrderedPathSet* fields;
        if (pn->proj.type() == projection_ast::ProjectType::kInclusion) {
            behaviour = sbe::MakeBsonObjStage::FieldBehavior::keep;
            fields = &pn->proj.getRequiredFields();
        } else {
            behaviour = sbe::MakeBsonObjStage::FieldBehavior::drop;
            fields = &pn->proj.getExcludedPaths();
        }

        outputs.set(kResult, _slotIdGenerator.generate());
        stage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(stage),
                                                  outputs.get(kResult),
                                                  childResult,
                                                  behaviour,
                                                  *fields,
                                                  OrderedPathSet{},
                                                  sbe::value::SlotVector{},
                                                  true,
                                                  false,
                                                  root->nodeId());
    }

    auto [outStage, nothingSlots] = projectNothingToSlots(
        std::move(stage), additionalFields.size(), root->nodeId(), &_slotIdGenerator);
    for (size_t i = 0; i < additionalFields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, std::move(additionalFields[i])),
                    nothingSlots[i]);
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(outStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionCovered(const QuerySolutionNode* root,
                                              const PlanStageReqs& reqs) {
    using namespace std::literals;
    tassert(6023406, "buildProjectionCovered() does not support kKey", !reqs.hasKeys());

    auto pn = static_cast<const ProjectionNodeCovered*>(root);
    invariant(pn->proj.isSimple());

    tassert(5037301,
            str::stream() << "Can't build covered projection for fetched sub-plan: "
                          << root->toString(),
            !pn->children[0]->fetched());

    // This is a ProjectionCoveredNode, so we will be pulling all the data we need from one index.
    // pn->coveredKeyObj is the "index.keyPattern" from the child (which is either an IndexScanNode
    // or DistinctNode). pn->coveredKeyObj lists all the fields that the index can provide, not the
    // fields that the projection wants. 'pn->proj.getRequiredFields()' lists all of the fields
    // that the projection needs. Since this is a simple covered projection, we're guaranteed that
    // 'pn->proj.getRequiredFields()' is a subset of pn->coveredKeyObj.

    // List out the projected fields in the order they appear in 'coveredKeyObj'.
    std::vector<std::string> keys;
    StringDataSet keysSet;
    for (auto&& elt : pn->coveredKeyObj) {
        std::string key(elt.fieldNameStringData());
        if (pn->proj.getRequiredFields().count(key)) {
            keys.emplace_back(std::move(key));
            keysSet.emplace(elt.fieldNameStringData());
        }
    }

    // The child must produce all of the slots required by the parent of this ProjectionNodeSimple,
    // except for 'resultSlot' which will be produced by the MakeBsonObjStage below if requested by
    // the caller. In addition to that, the child must produce the index key slots that are needed
    // by this covered projection.
    auto childReqs = reqs.copy().clear(kResult).clearAllFields().setKeys(keys);
    auto [stage, childOutputs] = build(pn->children[0].get(), childReqs);
    auto outputs = std::move(childOutputs);

    if (reqs.has(kResult)) {
        auto indexKeySlots = sbe::makeSV();
        std::vector<std::string> keyFieldNames;

        if (keysSet.count("_id"_sd)) {
            keyFieldNames.emplace_back("_id"_sd);
            indexKeySlots.emplace_back(outputs.get(std::make_pair(PlanStageSlots::kKey, "_id"_sd)));
        }

        for (const auto& key : keys) {
            if (key != "_id"_sd) {
                keyFieldNames.emplace_back(key);
                indexKeySlots.emplace_back(
                    outputs.get(std::make_pair(PlanStageSlots::kKey, StringData(key))));
            }
        }

        auto resultSlot = _slotIdGenerator.generate();
        stage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(stage),
                                                  resultSlot,
                                                  boost::none,
                                                  boost::none,
                                                  std::vector<std::string>{},
                                                  std::move(keyFieldNames),
                                                  std::move(indexKeySlots),
                                                  true,
                                                  false,
                                                  root->nodeId());

        outputs.set(kResult, resultSlot);
    }

    auto [fields, additionalFields] =
        splitVector(reqs.getFields(), [&](const std::string& s) { return keysSet.count(s); });
    for (size_t i = 0; i < fields.size(); ++i) {
        auto slot = outputs.get(std::make_pair(PlanStageSlots::kKey, StringData(fields[i])));
        outputs.set(std::make_pair(PlanStageSlots::kField, std::move(fields[i])), slot);
    }

    auto [outStage, nothingSlots] = projectNothingToSlots(
        std::move(stage), additionalFields.size(), root->nodeId(), &_slotIdGenerator);
    for (size_t i = 0; i < additionalFields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, std::move(additionalFields[i])),
                    nothingSlots[i]);
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(outStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionDefault(const QuerySolutionNode* root,
                                              const PlanStageReqs& reqs) {
    tassert(6023407, "buildProjectionDefault() does not support kKey", !reqs.hasKeys());

    auto pn = static_cast<const ProjectionNodeDefault*>(root);
    const auto& projection = pn->proj;

    // TODO SERVER-57533: Support multiple index scan nodes located below OR and SORT_MERGE stages.
    if (const auto [ixn, ct] = getFirstNodeByType(root, STAGE_IXSCAN);
        !pn->fetched() && projection.isInclusionOnly() && ixn && ct == 1) {
        return buildProjectionDefaultCovered(root, reqs);
    }

    // If the projection doesn't need the whole document, then we take all the top-level fields
    // referenced by expressions in the projection and we add them to 'fields'. At present, we
    // intentionally ignore any basic inclusions that are part of the projection (ex. {a:1})
    // for the purposes of populating 'fields'.
    DepsTracker deps;
    addProjectionExprDependencies(projection, &deps);
    auto fields =
        !deps.needWholeDocument ? getTopLevelFields(deps.fields) : std::vector<std::string>{};

    // The child must produce all of the slots required by the parent of this ProjectionNodeDefault.
    // In addition to that, the child must always produce 'kResult' because it's needed by the
    // projection logic below.
    auto childReqs = reqs.copy().set(kResult).clearAllFields().setFields(fields);

    auto [stage, outputs] = build(pn->children[0].get(), childReqs);

    auto relevantSlots = getSlotsToForward(childReqs, outputs);

    auto projectionExpr = generateProjection(_state, &projection, outputs.get(kResult), &outputs);
    auto [resultSlot, resultStage] = projectEvalExpr(std::move(projectionExpr),
                                                     EvalStage{std::move(stage), {}},
                                                     root->nodeId(),
                                                     &_slotIdGenerator);

    stage = resultStage.extractStage(root->nodeId());
    outputs.set(kResult, resultSlot);

    outputs.clearAllFields();
    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionDefaultCovered(const QuerySolutionNode* root,
                                                     const PlanStageReqs& reqs) {
    tassert(6023408, "buildProjectionDefaultCovered() does not support kKey", !reqs.hasKeys());

    auto pn = static_cast<const ProjectionNodeDefault*>(root);
    const auto& projection = pn->proj;

    tassert(7055402,
            "buildProjectionDefaultCovered() expected 'pn' to be an inclusion-only projection",
            projection.isInclusionOnly());
    tassert(
        7055403, "buildProjectionDefaultCovered() expected 'pn' to not be fetched", !pn->fetched());

    auto patternRoot = buildPatternTree(pn->proj);
    std::vector<std::string> keys;
    StringSet keysSet;
    std::vector<IndexKeyPatternTreeNode*> patternNodesForSlots;

    visitPatternTreeLeaves(&patternRoot, [&](const std::string& path, IndexKeyPatternTreeNode* n) {
        keys.emplace_back(path);
        keysSet.emplace(path);
        patternNodesForSlots.push_back(n);
    });

    auto childReqs = reqs.copy().clear(kResult).clearAllFields().setKeys(keys);

    auto [stage, outputs] = build(pn->children[0].get(), childReqs);

    auto [fields, additionalFields] =
        splitVector(reqs.getFields(), [&](const std::string& s) { return keysSet.count(s); });
    for (size_t i = 0; i < fields.size(); ++i) {
        auto slot = outputs.get(std::make_pair(PlanStageSlots::kKey, StringData(fields[i])));
        outputs.set(std::make_pair(PlanStageSlots::kField, std::move(fields[i])), slot);
    }

    if (reqs.has(kResult) || !additionalFields.empty()) {
        // Extract slots corresponding to each of the projection fieldpaths.
        for (size_t i = 0; i < keys.size(); i++) {
            patternNodesForSlots[i]->indexKeySlot =
                outputs.get(std::make_pair(PlanStageSlots::kKey, StringData(keys[i])));
        }

        // Finally, build the expression to create object with requested projection fieldpaths.
        auto resultSlot = _slotIdGenerator.generate();
        outputs.set(kResult, resultSlot);

        stage = sbe::makeProjectStage(
            std::move(stage), root->nodeId(), resultSlot, buildNewObjExpr(&patternRoot));
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildOr(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023409, "buildOr() does not support kKey", !reqs.hasKeys());

    auto orn = static_cast<const OrNode*>(root);

    // Children must produce all of the slots required by the parent of this OrNode. In addition
    // to that, children must always produce a 'recordIdSlot' if the 'dedup' flag is true, and
    // children must always produce a 'resultSlot' if 'filter' is non-null.
    auto childReqs = reqs.copy().setIf(kResult, orn->filter.get()).setIf(kRecordId, orn->dedup);

    auto fields = reqs.getFields();

    if (orn->filter) {
        DepsTracker deps;
        match_expression::addDependencies(orn->filter.get(), &deps);
        // If the filter predicate doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!deps.needWholeDocument) {
            auto topLevelFields = getTopLevelFields(deps.fields);
            fields = appendVectorUnique(std::move(fields), std::move(topLevelFields));
        }
    }

    childReqs.setFields(fields);

    sbe::PlanStage::Vector inputStages;
    std::vector<sbe::value::SlotVector> inputSlots;
    for (auto&& child : orn->children) {
        auto [stage, outputs] = build(child.get(), childReqs);

        inputStages.emplace_back(std::move(stage));
        inputSlots.emplace_back(getSlotsToForward(childReqs, outputs));
    }

    // Construct a union stage whose branches are translated children of the 'Or' node.
    PlanStageSlots outputs(childReqs, &_slotIdGenerator);
    auto unionOutputSlots = getSlotsToForward(childReqs, outputs);

    auto stage = sbe::makeS<sbe::UnionStage>(
        std::move(inputStages), std::move(inputSlots), std::move(unionOutputSlots), root->nodeId());

    if (orn->dedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(kRecordId)), root->nodeId());
        // Stop propagating the RecordId output if none of our ancestors are going to use it.
        if (!reqs.has(kRecordId)) {
            outputs.clear(kRecordId);
        }
    }

    if (orn->filter) {
        auto forwardingReqs = reqs.copy().set(kResult).setFields(std::move(fields));
        auto relevantSlots = getSlotsToForward(forwardingReqs, outputs);

        auto [_, outputStage] = generateFilter(_state,
                                               orn->filter.get(),
                                               {std::move(stage), std::move(relevantSlots)},
                                               outputs.get(kResult),
                                               &outputs,
                                               root->nodeId());
        stage = outputStage.extractStage(root->nodeId());
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildTextMatch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto textNode = static_cast<const TextMatchNode*>(root);
    const auto& coll = getCurrentCollection(reqs);
    tassert(5432212, "no collection object", coll);
    tassert(6023410, "buildTextMatch() does not support kKey", !reqs.hasKeys());
    tassert(5432215,
            str::stream() << "text match node must have one child, but got "
                          << root->children.size(),
            root->children.size() == 1);
    // TextMatchNode guarantees to produce a fetched sub-plan, but it doesn't fetch itself. Instead,
    // its child sub-plan must be fully fetched, and a text match plan is constructed under this
    // assumption.
    tassert(5432216, "text match input must be fetched", root->children[0]->fetched());

    auto childReqs = reqs.copy().set(kResult);
    auto [stage, outputs] = build(textNode->children[0].get(), childReqs);
    tassert(5432217, "result slot is not produced by text match sub-plan", outputs.has(kResult));

    // Create an FTS 'matcher' to apply 'ftsQuery' to matching documents.
    auto matcher = makeFtsMatcher(
        _opCtx, coll, textNode->index.identifier.catalogName, textNode->ftsQuery.get());

    // Build an 'ftsMatch' expression to match a document stored in the 'kResult' slot using the
    // 'matcher' instance.
    auto ftsMatch =
        makeFunction("ftsMatch",
                     makeConstant(sbe::value::TypeTags::ftsMatcher,
                                  sbe::value::bitcastFrom<fts::FTSMatcher*>(matcher.release())),
                     makeVariable(outputs.get(kResult)));

    // Wrap the 'ftsMatch' expression into an 'if' expression to ensure that it can be applied only
    // to a document.
    auto filter =
        sbe::makeE<sbe::EIf>(makeFunction("isObject", makeVariable(outputs.get(kResult))),
                             std::move(ftsMatch),
                             sbe::makeE<sbe::EFail>(ErrorCodes::Error{4623400},
                                                    "textmatch requires input to be an object"));

    // Add a filter stage to apply 'ftsQuery' to matching documents and discard documents which do
    // not match.
    stage =
        sbe::makeS<sbe::FilterStage<false>>(std::move(stage), std::move(filter), root->nodeId());

    if (reqs.has(kReturnKey)) {
        // Assign the 'returnKeySlot' to be the empty object.
        outputs.set(kReturnKey, _slotIdGenerator.generate());
        stage = sbe::makeProjectStage(
            std::move(stage), root->nodeId(), outputs.get(kReturnKey), makeFunction("newObj"));
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildReturnKey(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023411, "buildReturnKey() does not support kKey", !reqs.hasKeys());

    // TODO SERVER-49509: If the projection includes {$meta: "sortKey"}, the result of this stage
    // should also include the sort key. Everything else in the projection is ignored.
    auto returnKeyNode = static_cast<const ReturnKeyNode*>(root);

    // The child must produce all of the slots required by the parent of this ReturnKeyNode except
    // for 'resultSlot'. In addition to that, the child must always produce a 'returnKeySlot'.
    // After build() returns, we take the 'returnKeySlot' produced by the child and store it into
    // 'resultSlot' for the parent of this ReturnKeyNode to consume.
    auto childReqs = reqs.copy().clear(kResult).clearAllFields().set(kReturnKey);
    auto [stage, outputs] = build(returnKeyNode->children[0].get(), childReqs);

    outputs.set(kResult, outputs.get(kReturnKey));
    outputs.clear(kReturnKey);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildEof(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    return generateEofPlan(root->nodeId(), reqs, &_slotIdGenerator);
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildAndHash(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto andHashNode = static_cast<const AndHashNode*>(root);

    tassert(6023412, "buildAndHash() does not support kKey", !reqs.hasKeys());
    tassert(5073711, "need at least two children for AND_HASH", andHashNode->children.size() >= 2);

    auto childReqs = reqs.copy().set(kResult).set(kRecordId).clearAllFields();

    auto outerChild = andHashNode->children[0].get();
    auto innerChild = andHashNode->children[1].get();

    auto [outerStage, outerOutputs] = build(outerChild, childReqs);
    auto outerIdSlot = outerOutputs.get(kRecordId);
    auto outerResultSlot = outerOutputs.get(kResult);
    auto outerCondSlots = sbe::makeSV(outerIdSlot);
    auto outerProjectSlots = sbe::makeSV(outerResultSlot);

    auto [innerStage, innerOutputs] = build(innerChild, childReqs);
    tassert(5073712, "innerOutputs must contain kRecordId slot", innerOutputs.has(kRecordId));
    tassert(5073713, "innerOutputs must contain kResult slot", innerOutputs.has(kResult));
    auto innerIdSlot = innerOutputs.get(kRecordId);
    auto innerResultSlot = innerOutputs.get(kResult);
    auto innerSnapshotIdSlot = innerOutputs.getIfExists(kSnapshotId);
    auto innerIndexIdSlot = innerOutputs.getIfExists(kIndexId);
    auto innerIndexKeySlot = innerOutputs.getIfExists(kIndexKey);
    auto innerIndexKeyPatternSlot = innerOutputs.getIfExists(kIndexKeyPattern);

    auto innerCondSlots = sbe::makeSV(innerIdSlot);
    auto innerProjectSlots = sbe::makeSV(innerResultSlot);

    auto collatorSlot = _data.env->getSlotIfExists("collator"_sd);

    // Designate outputs.
    PlanStageSlots outputs;

    outputs.set(kResult, innerResultSlot);

    if (reqs.has(kRecordId)) {
        outputs.set(kRecordId, innerIdSlot);
    }
    if (reqs.has(kSnapshotId) && innerSnapshotIdSlot) {
        auto slot = *innerSnapshotIdSlot;
        innerProjectSlots.push_back(slot);
        outputs.set(kSnapshotId, slot);
    }
    if (reqs.has(kIndexId) && innerIndexIdSlot) {
        auto slot = *innerIndexIdSlot;
        innerProjectSlots.push_back(slot);
        outputs.set(kIndexId, slot);
    }
    if (reqs.has(kIndexKey) && innerIndexKeySlot) {
        auto slot = *innerIndexKeySlot;
        innerProjectSlots.push_back(slot);
        outputs.set(kIndexKey, slot);
    }
    if (reqs.has(kIndexKeyPattern) && innerIndexKeyPatternSlot) {
        auto slot = *innerIndexKeyPatternSlot;
        innerProjectSlots.push_back(slot);
        outputs.set(kIndexKeyPattern, slot);
    }

    auto stage = sbe::makeS<sbe::HashJoinStage>(std::move(outerStage),
                                                std::move(innerStage),
                                                outerCondSlots,
                                                outerProjectSlots,
                                                innerCondSlots,
                                                innerProjectSlots,
                                                collatorSlot,
                                                root->nodeId());

    // If there are more than 2 children, iterate all remaining children and hash
    // join together.
    for (size_t i = 2; i < andHashNode->children.size(); i++) {
        auto [childStage, outputs] = build(andHashNode->children[i].get(), childReqs);
        tassert(5073714, "outputs must contain kRecordId slot", outputs.has(kRecordId));
        tassert(5073715, "outputs must contain kResult slot", outputs.has(kResult));
        auto idSlot = outputs.get(kRecordId);
        auto resultSlot = outputs.get(kResult);
        auto condSlots = sbe::makeSV(idSlot);
        auto projectSlots = sbe::makeSV(resultSlot);

        // The previous HashJoinStage is always set as the inner stage, so that we can reuse the
        // innerIdSlot and innerResultSlot that have been designated as outputs.
        stage = sbe::makeS<sbe::HashJoinStage>(std::move(childStage),
                                               std::move(stage),
                                               condSlots,
                                               projectSlots,
                                               innerCondSlots,
                                               innerProjectSlots,
                                               collatorSlot,
                                               root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildAndSorted(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023413, "buildAndSorted() does not support kKey", !reqs.hasKeys());

    auto andSortedNode = static_cast<const AndSortedNode*>(root);

    // Need at least two children.
    tassert(
        5073706, "need at least two children for AND_SORTED", andSortedNode->children.size() >= 2);

    auto childReqs = reqs.copy().set(kResult).set(kRecordId).clearAllFields();

    auto outerChild = andSortedNode->children[0].get();
    auto innerChild = andSortedNode->children[1].get();

    auto [outerStage, outerOutputs] = build(outerChild, childReqs);
    auto outerIdSlot = outerOutputs.get(kRecordId);
    auto outerResultSlot = outerOutputs.get(kResult);

    auto outerKeySlots = sbe::makeSV(outerIdSlot);
    auto outerProjectSlots = sbe::makeSV(outerResultSlot);
    if (outerOutputs.has(kSnapshotId)) {
        outerProjectSlots.push_back(outerOutputs.get(kSnapshotId));
    }

    if (outerOutputs.has(kIndexId)) {
        outerProjectSlots.push_back(outerOutputs.get(kIndexId));
    }

    if (outerOutputs.has(kIndexKey)) {
        outerProjectSlots.push_back(outerOutputs.get(kIndexKey));
    }

    if (outerOutputs.has(kIndexKeyPattern)) {
        outerProjectSlots.push_back(outerOutputs.get(kIndexKeyPattern));
    }

    auto [innerStage, innerOutputs] = build(innerChild, childReqs);
    tassert(5073707, "innerOutputs must contain kRecordId slot", innerOutputs.has(kRecordId));
    tassert(5073708, "innerOutputs must contain kResult slot", innerOutputs.has(kResult));
    auto innerIdSlot = innerOutputs.get(kRecordId);
    auto innerResultSlot = innerOutputs.get(kResult);

    auto innerKeySlots = sbe::makeSV(innerIdSlot);
    auto innerProjectSlots = sbe::makeSV(innerResultSlot);

    // Designate outputs.
    PlanStageSlots outputs;

    outputs.set(kResult, innerResultSlot);

    if (reqs.has(kRecordId)) {
        outputs.set(kRecordId, innerIdSlot);
    }
    if (reqs.has(kSnapshotId)) {
        auto innerSnapshotSlot = innerOutputs.get(kSnapshotId);
        innerProjectSlots.push_back(innerSnapshotSlot);
        outputs.set(kSnapshotId, innerSnapshotSlot);
    }
    if (reqs.has(kIndexId)) {
        auto innerIndexIdSlot = innerOutputs.get(kIndexId);
        innerProjectSlots.push_back(innerIndexIdSlot);
        outputs.set(kIndexId, innerIndexIdSlot);
    }
    if (reqs.has(kIndexKey)) {
        auto innerIndexKeySlot = innerOutputs.get(kIndexKey);
        innerProjectSlots.push_back(innerIndexKeySlot);
        outputs.set(kIndexKey, innerIndexKeySlot);
    }
    if (reqs.has(kIndexKeyPattern)) {
        auto innerIndexKeyPatternSlot = innerOutputs.get(kIndexKeyPattern);
        innerProjectSlots.push_back(innerIndexKeyPatternSlot);
        outputs.set(kIndexKeyPattern, innerIndexKeyPatternSlot);
    }

    std::vector<sbe::value::SortDirection> sortDirs(outerKeySlots.size(),
                                                    sbe::value::SortDirection::Ascending);

    auto stage = sbe::makeS<sbe::MergeJoinStage>(std::move(outerStage),
                                                 std::move(innerStage),
                                                 outerKeySlots,
                                                 outerProjectSlots,
                                                 innerKeySlots,
                                                 innerProjectSlots,
                                                 sortDirs,
                                                 root->nodeId());

    // If there are more than 2 children, iterate all remaining children and merge
    // join together.
    for (size_t i = 2; i < andSortedNode->children.size(); i++) {
        auto [childStage, outputs] = build(andSortedNode->children[i].get(), childReqs);
        tassert(5073709, "outputs must contain kRecordId slot", outputs.has(kRecordId));
        tassert(5073710, "outputs must contain kResult slot", outputs.has(kResult));
        auto idSlot = outputs.get(kRecordId);
        auto resultSlot = outputs.get(kResult);
        auto keySlots = sbe::makeSV(idSlot);
        auto projectSlots = sbe::makeSV(resultSlot);

        stage = sbe::makeS<sbe::MergeJoinStage>(std::move(childStage),
                                                std::move(stage),
                                                keySlots,
                                                projectSlots,
                                                innerKeySlots,
                                                innerProjectSlots,
                                                sortDirs,
                                                root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

namespace {
template <typename F>
struct FieldPathAndCondPreVisitor : public SelectiveConstExpressionVisitorBase {
    // To avoid overloaded-virtual warnings.
    using SelectiveConstExpressionVisitorBase::visit;

    FieldPathAndCondPreVisitor(const F& fn, int32_t& nestedCondLevel)
        : _fn(fn), _nestedCondLevel(nestedCondLevel) {}

    void visit(const ExpressionFieldPath* expr) final {
        _fn(expr, _nestedCondLevel);
    }

    void visit(const ExpressionCond* expr) final {
        ++_nestedCondLevel;
    }

    void visit(const ExpressionSwitch* expr) final {
        ++_nestedCondLevel;
    }

    void visit(const ExpressionIfNull* expr) final {
        ++_nestedCondLevel;
    }

    void visit(const ExpressionAnd* expr) final {
        ++_nestedCondLevel;
    }

    void visit(const ExpressionOr* expr) final {
        ++_nestedCondLevel;
    }

    F _fn;
    // Tracks the number of conditional expressions like $cond or $ifNull that are above us in the
    // tree.
    int32_t& _nestedCondLevel;
};

struct CondPostVisitor : public SelectiveConstExpressionVisitorBase {
    // To avoid overloaded-virtual warnings.
    using SelectiveConstExpressionVisitorBase::visit;

    CondPostVisitor(int32_t& nestedCondLevel) : _nestedCondLevel(nestedCondLevel) {}

    void visit(const ExpressionCond* expr) final {
        --_nestedCondLevel;
    }

    void visit(const ExpressionSwitch* expr) final {
        --_nestedCondLevel;
    }

    void visit(const ExpressionIfNull* expr) final {
        --_nestedCondLevel;
    }

    void visit(const ExpressionAnd* expr) final {
        --_nestedCondLevel;
    }

    void visit(const ExpressionOr* expr) final {
        --_nestedCondLevel;
    }

    int32_t& _nestedCondLevel;
};

/**
 * Walks through the 'expr' expression tree and whenever finds an 'ExpressionFieldPath', calls
 * the 'fn' function. Type requirement for 'fn' is it must have a const 'ExpressionFieldPath'
 * pointer parameter and 'nestedCondLevel' parameter.
 */
template <typename F>
void walkAndActOnFieldPaths(Expression* expr, const F& fn) {
    int32_t nestedCondLevel = 0;
    FieldPathAndCondPreVisitor<F> preVisitor(fn, nestedCondLevel);
    CondPostVisitor postVisitor(nestedCondLevel);
    ExpressionWalker walker(&preVisitor, nullptr /*inVisitor*/, &postVisitor);
    expression_walker::walk(expr, &walker);
}

/**
 * If there are adjacent $group stages in a pipeline and two $group stages are pushed down together,
 * the first $group becomes a child GROUP node and the second $group becomes a parent GROUP node in
 * a query solution tree. In the case that all field paths are top-level fields for the parent GROUP
 * node, we can skip the mkbson stage of the child GROUP node and instead, the child GROUP node
 * returns top-level fields and their associated slots. If a field path is found in child outputs,
 * we should replace getField() by the associated slot because there's no object on which we can
 * call getField().
 *
 * Also deduplicates field lookups for a field path which is accessed multiple times.
 */
EvalStage optimizeFieldPaths(StageBuilderState& state,
                             const boost::intrusive_ptr<Expression>& expr,
                             EvalStage stage,
                             const PlanStageSlots& outputs,
                             PlanNodeId nodeId) {
    using namespace fmt::literals;
    auto rootSlot = outputs.getIfExists(PlanStageSlots::kResult);

    walkAndActOnFieldPaths(expr.get(), [&](const ExpressionFieldPath* fieldExpr, int32_t) {
        // We optimize neither a field path for the top-level document itself nor a field path that
        // refers to a variable instead of calling getField().
        if (fieldExpr->getFieldPath().getPathLength() == 1 || fieldExpr->isVariableReference()) {
            return;
        }

        auto fieldPathStr = fieldExpr->getFieldPath().fullPath();

        if (!state.preGeneratedExprs.contains(fieldPathStr)) {
            auto rootExpr = rootSlot.has_value() ? EvalExpr{*rootSlot} : EvalExpr{};
            auto expr = generateExpression(state, fieldExpr, std::move(rootExpr), &outputs);

            auto [slot, projectStage] =
                projectEvalExpr(std::move(expr), std::move(stage), nodeId, state.slotIdGenerator);

            state.preGeneratedExprs.emplace(fieldPathStr, slot);
            stage = std::move(projectStage);
        }
    });

    return stage;
}

EvalExprStagePair generateGroupByKeyImpl(StageBuilderState& state,
                                         const boost::intrusive_ptr<Expression>& idExpr,
                                         const PlanStageSlots& outputs,
                                         const boost::optional<sbe::value::SlotId>& rootSlot,
                                         EvalStage stage,
                                         PlanNodeId nodeId,
                                         sbe::value::SlotIdGenerator* slotIdGenerator) {
    // Optimize field paths before generating the expression.
    stage = optimizeFieldPaths(state, idExpr, std::move(stage), outputs, nodeId);

    auto rootExpr = rootSlot.has_value() ? EvalExpr{*rootSlot} : EvalExpr{};
    auto expr = generateExpression(state, idExpr.get(), std::move(rootExpr), &outputs);

    return {std::move(expr), std::move(stage)};
}

std::tuple<sbe::value::SlotVector, EvalStage, std::unique_ptr<sbe::EExpression>> generateGroupByKey(
    StageBuilderState& state,
    const boost::intrusive_ptr<Expression>& idExpr,
    const PlanStageSlots& outputs,
    EvalStage stage,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    auto rootSlot = outputs.getIfExists(PlanStageSlots::kResult);

    if (auto idExprObj = dynamic_cast<ExpressionObject*>(idExpr.get()); idExprObj) {
        sbe::value::SlotVector slots;
        sbe::EExpression::Vector exprs;

        for (auto&& [fieldName, fieldExpr] : idExprObj->getChildExpressions()) {
            auto [groupByEvalExpr, groupByEvalStage] = generateGroupByKeyImpl(
                state, fieldExpr, outputs, rootSlot, std::move(stage), nodeId, slotIdGenerator);

            auto [slot, projectStage] = projectEvalExpr(
                std::move(groupByEvalExpr), std::move(groupByEvalStage), nodeId, slotIdGenerator);

            slots.push_back(slot);
            groupByEvalExpr = slot;
            stage = std::move(projectStage);

            exprs.emplace_back(makeConstant(fieldName));
            exprs.emplace_back(groupByEvalExpr.extractExpr());
        }

        // When there's only one field in the document _id expression, 'Nothing' is converted to
        // 'Null'.
        // TODO SERVER-21992: Remove the following block because this block emulates the classic
        // engine's buggy behavior. With index that can handle 'Nothing' and 'Null' differently,
        // SERVER-21992 issue goes away and the distinct scan should be able to return 'Nothing' and
        // 'Null' separately.
        if (slots.size() == 1) {
            auto [slot, projectStage] = projectEvalExpr(
                makeFillEmptyNull(std::move(exprs[1])), std::move(stage), nodeId, slotIdGenerator);
            slots[0] = slot;
            exprs[1] = makeVariable(slots[0]);
            stage = std::move(projectStage);
        }

        // Composes the _id document and assigns a slot to the result using 'newObj' function if _id
        // should produce a document. For example, resultSlot = newObj(field1, slot1, ..., fieldN,
        // slotN)
        return {slots, std::move(stage), sbe::makeE<sbe::EFunction>("newObj"_sd, std::move(exprs))};
    }

    auto [groupByEvalExpr, groupByEvalStage] = generateGroupByKeyImpl(
        state, idExpr, outputs, rootSlot, std::move(stage), nodeId, slotIdGenerator);

    // The group-by field may end up being 'Nothing' and in that case _id: null will be
    // returned. Calling 'makeFillEmptyNull' for the group-by field takes care of that.
    auto fillEmptyNullExpr = makeFillEmptyNull(groupByEvalExpr.extractExpr());
    auto [slot, projectStage] = projectEvalExpr(
        std::move(fillEmptyNullExpr), std::move(groupByEvalStage), nodeId, slotIdGenerator);
    stage = std::move(projectStage);

    return {sbe::value::SlotVector{slot}, std::move(stage), nullptr};
}

std::tuple<sbe::value::SlotVector, EvalStage> generateAccumulator(
    StageBuilderState& state,
    const AccumulationStatement& accStmt,
    EvalStage stage,
    const PlanStageSlots& outputs,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>>& accSlotToExprMap) {
    auto rootSlot = outputs.getIfExists(PlanStageSlots::kResult);

    // Input fields may need field traversal.
    stage = optimizeFieldPaths(state, accStmt.expr.argument, std::move(stage), outputs, nodeId);
    auto rootExpr = rootSlot.has_value() ? EvalExpr{*rootSlot} : EvalExpr{};
    auto argExpr =
        generateExpression(state, accStmt.expr.argument.get(), std::move(rootExpr), &outputs);

    // One accumulator may be translated to multiple accumulator expressions. For example, The
    // $avg will have two accumulators expressions, a sum(..) and a count which is implemented
    // as sum(1).
    auto collatorSlot = state.data->env->getSlotIfExists("collator"_sd);
    auto accExprs = stage_builder::buildAccumulator(
        accStmt, argExpr.extractExpr(), collatorSlot, *state.frameIdGenerator);

    sbe::value::SlotVector aggSlots;
    for (auto& accExpr : accExprs) {
        auto slot = slotIdGenerator->generate();
        aggSlots.push_back(slot);
        accSlotToExprMap.emplace(slot, std::move(accExpr));
    }

    return {std::move(aggSlots), std::move(stage)};
}

std::tuple<std::vector<std::string>, sbe::value::SlotVector, EvalStage> generateGroupFinalStage(
    StageBuilderState& state,
    EvalStage groupEvalStage,
    std::unique_ptr<sbe::EExpression> idDocExpr,
    sbe::value::SlotVector dedupedGroupBySlots,
    const std::vector<AccumulationStatement>& accStmts,
    const std::vector<sbe::value::SlotVector>& aggSlotsVec,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> prjSlotToExprMap;
    sbe::value::SlotVector groupOutSlots{groupEvalStage.getOutSlots()};
    // To passthrough the output slots of accumulators with trivial finalizers, we need to find
    // their slot ids. We can do this by sorting 'groupEvalStage.outSlots' because the slot ids
    // correspond to the order in which the accumulators were translated (that is, the order in
    // which they are listed in 'accStmts'). Note, that 'groupEvalStage.outSlots' contains deduped
    // group-by slots at the front and the accumulator slots at the back.
    std::sort(groupOutSlots.begin() + dedupedGroupBySlots.size(), groupOutSlots.end());

    tassert(5995100,
            "The _id expression must either produce a document or a scalar value",
            idDocExpr || dedupedGroupBySlots.size() == 1);

    auto finalGroupBySlot = [&]() {
        if (!idDocExpr) {
            return dedupedGroupBySlots[0];
        } else {
            auto slot = slotIdGenerator->generate();
            prjSlotToExprMap.emplace(slot, std::move(idDocExpr));
            return slot;
        }
    }();

    auto finalSlots{sbe::value::SlotVector{finalGroupBySlot}};
    std::vector<std::string> fieldNames{"_id"};
    size_t idxAccFirstSlot = dedupedGroupBySlots.size();
    for (size_t idxAcc = 0; idxAcc < accStmts.size(); ++idxAcc) {
        // Gathers field names for the output object from accumulator statements.
        fieldNames.push_back(accStmts[idxAcc].fieldName);
        auto finalExpr = stage_builder::buildFinalize(state, accStmts[idxAcc], aggSlotsVec[idxAcc]);

        // The final step may not return an expression if it's trivial. For example, $first and
        // $last's final steps are trivial.
        if (finalExpr) {
            auto outSlot = slotIdGenerator->generate();
            finalSlots.push_back(outSlot);
            prjSlotToExprMap.emplace(outSlot, std::move(finalExpr));
        } else {
            finalSlots.push_back(groupOutSlots[idxAccFirstSlot]);
        }

        // Some accumulator(s) like $avg generate multiple expressions and slots. So, need to
        // advance this index by the number of those slots for each accumulator.
        idxAccFirstSlot += aggSlotsVec[idxAcc].size();
    }

    // Gathers all accumulator results. If there're no project expressions, does not add a project
    // stage.
    auto retEvalStage = prjSlotToExprMap.empty()
        ? std::move(groupEvalStage)
        : makeProject(std::move(groupEvalStage), std::move(prjSlotToExprMap), nodeId);

    return {std::move(fieldNames), std::move(finalSlots), std::move(retEvalStage)};
}

sbe::value::SlotVector dedupGroupBySlots(const sbe::value::SlotVector& groupBySlots) {
    stdx::unordered_set<sbe::value::SlotId> uniqueSlots;
    sbe::value::SlotVector dedupedGroupBySlots;

    for (auto slot : groupBySlots) {
        if (!uniqueSlots.contains(slot)) {
            dedupedGroupBySlots.push_back(slot);
            uniqueSlots.insert(slot);
        }
    }

    return dedupedGroupBySlots;
}
}  // namespace

/**
 * Translates a 'GroupNode' QSN into a sbe::PlanStage tree. This translation logic assumes that the
 * only child of the 'GroupNode' must return an Object (or 'BSONObject') and the translated sub-tree
 * must return 'BSONObject'. The returned 'BSONObject' will always have an "_id" field for the group
 * key and zero or more field(s) for accumulators.
 *
 * For example, a QSN tree: GroupNode(nodeId=2) over a VirtualScanNode(nodeId=1), we would have the
 * following translated sbe::PlanStage tree. In this example, we assume that the $group pipeline
 * spec is {"_id": "$a", "x": {"$min": "$b"}, "y": {"$first": "$b"}}.
 *
 * [2] mkbson s14 [_id = s9, x = s14, y = s13] true false
 * [2] project [s14 = fillEmpty (s11, null)]
 * [2] group [s9] [s12 = min (if (! exists (s9) || typeMatch (s9, 0x00000440), Nothing, s9)),
 *                 s13 = first (fillEmpty (s10, null))]
 * [2] project [s11 = getField (s7, "b")]
 * [2] project [s10 = getField (s7, "b")]
 * [2] project [s9 = fillEmpty (s8, null)]
 * [2] project [s8 = getField (s7, "a")]
 * [1] project [s7 = getElement (s5, 0)]
 * [1] unwind s5 s6 s4 false
 * [1] project [s4 = [[{"a" : 1, "b" : 1}], [{"a" : 1, "b" : 2}], [{"a" : 2, "b" : 3}]]]
 * [1] limit 1 [1] coscan
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildGroup(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace fmt::literals;
    tassert(6023414, "buildGroup() does not support kKey", !reqs.hasKeys());

    auto groupNode = static_cast<const GroupNode*>(root);
    auto nodeId = groupNode->nodeId();
    const auto& idExpr = groupNode->groupByExpression;

    tassert(
        5851600, "should have one and only one child for GROUP", groupNode->children.size() == 1);
    tassert(5851601, "GROUP should have had group-by key expression", idExpr);
    tassert(
        6360401,
        "GROUP cannot propagate a record id slot, but the record id was requested by the parent",
        !reqs.has(kRecordId));

    const auto& childNode = groupNode->children[0].get();
    const auto& accStmts = groupNode->accumulators;

    auto childReqs = reqs.copy().set(kResult).clearAllFields();

    // If the group node doesn't need the whole document, then we take all the top-level fields
    // referenced by the group node and we add them to 'childReqs'.
    if (!groupNode->needWholeDocument) {
        childReqs.setFields(getTopLevelFields(groupNode->requiredFields));
    }

    // If the child is a GROUP and we can get everything we need from top-level field slots, then
    // we can avoid unnecessary materialization and not request the kResult slot from the child.
    if (childNode->getType() == StageType::STAGE_GROUP && !groupNode->needWholeDocument &&
        !containsPoisonTopLevelField(groupNode->requiredFields)) {
        childReqs.clear(kResult);
    }

    // Builds the child and gets the child result slot.
    auto [childStage, childOutputs] = build(childNode, childReqs);

    tassert(6075900,
            "Expected no optimized expressions but got: {}"_format(_state.preGeneratedExprs.size()),
            _state.preGeneratedExprs.empty());

    // Translates the group-by expression and wraps it with 'fillEmpty(..., null)' because the
    // missing field value for _id should be mapped to 'Null'.
    auto forwardingReqs = childReqs.copy().setIf(kResult, childOutputs.has(kResult));
    auto childEvalStage =
        EvalStage{std::move(childStage), getSlotsToForward(forwardingReqs, childOutputs)};

    auto [groupBySlots, groupByEvalStage, idDocExpr] = generateGroupByKey(
        _state, idExpr, childOutputs, std::move(childEvalStage), nodeId, &_slotIdGenerator);

    // Translates accumulators which are executed inside the group stage and gets slots for
    // accumulators.
    stage_builder::EvalStage accProjEvalStage = std::move(groupByEvalStage);
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> accSlotToExprMap;
    std::vector<sbe::value::SlotVector> aggSlotsVec;
    for (const auto& accStmt : accStmts) {
        auto [aggSlots, tempEvalStage] = generateAccumulator(_state,
                                                             accStmt,
                                                             std::move(accProjEvalStage),
                                                             childOutputs,
                                                             nodeId,
                                                             &_slotIdGenerator,
                                                             accSlotToExprMap);
        aggSlotsVec.emplace_back(std::move(aggSlots));
        accProjEvalStage = std::move(tempEvalStage);
    }

    // There might be duplicated expressions and slots. Dedup them before creating a HashAgg
    // because it would complain about duplicated slots and refuse to be created, which is
    // reasonable because duplicated expressions would not contribute to grouping.
    auto dedupedGroupBySlots = dedupGroupBySlots(groupBySlots);
    // Builds a group stage with accumulator expressions and group-by slot(s).
    auto groupEvalStage = makeHashAgg(std::move(accProjEvalStage),
                                      dedupedGroupBySlots,
                                      std::move(accSlotToExprMap),
                                      _state.data->env->getSlotIfExists("collator"_sd),
                                      _cq.getExpCtx()->allowDiskUse,
                                      nodeId);

    tassert(
        5851603,
        "Group stage's output slots must include deduped slots for group-by keys and slots for all "
        "accumulators",
        groupEvalStage.getOutSlots().size() ==
            std::accumulate(aggSlotsVec.begin(),
                            aggSlotsVec.end(),
                            dedupedGroupBySlots.size(),
                            [](int sum, const auto& aggSlots) { return sum + aggSlots.size(); }));
    tassert(5851604,
            "Group stage's output slots must contain the deduped groupBySlots at the front",
            std::equal(dedupedGroupBySlots.begin(),
                       dedupedGroupBySlots.end(),
                       groupEvalStage.getOutSlots().begin()));

    // Builds the final stage(s) over the collected accumulators.
    auto [fieldNames, finalSlots, groupFinalEvalStage] =
        generateGroupFinalStage(_state,
                                std::move(groupEvalStage),
                                std::move(idDocExpr),
                                dedupedGroupBySlots,
                                accStmts,
                                aggSlotsVec,
                                nodeId,
                                &_slotIdGenerator);
    auto stage = groupFinalEvalStage.extractStage(nodeId);

    tassert(5851605,
            "The number of final slots must be as 1 (the final group-by slot) + the number of acc "
            "slots",
            finalSlots.size() == 1 + accStmts.size());

    // Cleans up optimized expressions.
    _state.preGeneratedExprs.clear();

    auto fieldNamesSet = StringDataSet{fieldNames.begin(), fieldNames.end()};
    auto [fields, additionalFields] =
        splitVector(reqs.getFields(), [&](const std::string& s) { return fieldNamesSet.count(s); });
    auto fieldsSet = StringDataSet{fields.begin(), fields.end()};

    PlanStageSlots outputs;
    for (size_t i = 0; i < fieldNames.size(); ++i) {
        if (fieldsSet.count(fieldNames[i])) {
            outputs.set(std::make_pair(PlanStageSlots::kField, fieldNames[i]), finalSlots[i]);
        }
    };

    auto [outStage, nothingSlots] = projectNothingToSlots(
        std::move(stage), additionalFields.size(), root->nodeId(), &_slotIdGenerator);
    for (size_t i = 0; i < additionalFields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, std::move(additionalFields[i])),
                    nothingSlots[i]);
    }

    // Builds a outStage to create a result object out of a group-by slot and gathered accumulator
    // result slots if the parent node requests so.
    if (reqs.has(kResult)) {
        auto resultSlot = _slotIdGenerator.generate();
        outputs.set(kResult, resultSlot);
        // This mkbson stage combines 'finalSlots' into a bsonObject result slot which has
        // 'fieldNames' fields.
        if (groupNode->shouldProduceBson) {
            outStage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(outStage),
                                                         resultSlot,   // objSlot
                                                         boost::none,  // rootSlot
                                                         boost::none,  // fieldBehavior
                                                         std::vector<std::string>{},  // fields
                                                         std::move(fieldNames),  // projectFields
                                                         std::move(finalSlots),  // projectVars
                                                         true,                   // forceNewObject
                                                         false,                  // returnOldObject
                                                         nodeId);
        } else {
            outStage = sbe::makeS<sbe::MakeObjStage>(std::move(outStage),
                                                     resultSlot,                  // objSlot
                                                     boost::none,                 // rootSlot
                                                     boost::none,                 // fieldBehavior
                                                     std::vector<std::string>{},  // fields
                                                     std::move(fieldNames),       // projectFields
                                                     std::move(finalSlots),       // projectVars
                                                     true,                        // forceNewObject
                                                     false,                       // returnOldObject
                                                     nodeId);
        }
    }

    return {std::move(outStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::makeUnionForTailableCollScan(const QuerySolutionNode* root,
                                                    const PlanStageReqs& reqs) {
    using namespace std::literals;
    tassert(6023415, "makeUnionForTailableCollScan() does not support kKey", !reqs.hasKeys());

    // Register a SlotId in the global environment which would contain a recordId to resume a
    // tailable collection scan from. A PlanStage executor will track the last seen recordId and
    // will reset a SlotAccessor for the resumeRecordIdSlot with this recordId.
    auto resumeRecordIdSlot = _data.env->registerSlot(
        "resumeRecordId"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);

    // For tailable collection scan we need to build a special union sub-tree consisting of two
    // branches:
    //   1) An anchor branch implementing an initial collection scan before the first EOF is hit.
    //   2) A resume branch implementing all consecutive collection scans from a recordId which was
    //      seen last.
    //
    // The 'makeStage' parameter is used to build a PlanStage tree which is served as a root stage
    // for each of the union branches. The same mechanism is used to build each union branch, and
    // the special logic which needs to be triggered depending on which branch we build is
    // controlled by setting the isTailableCollScanResumeBranch flag in PlanStageReqs.
    auto makeUnionBranch = [&](bool isTailableCollScanResumeBranch)
        -> std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> {
        auto childReqs = reqs;
        childReqs.setIsTailableCollScanResumeBranch(isTailableCollScanResumeBranch);
        auto [branch, outputs] = build(root, childReqs);

        auto branchSlots = getSlotsToForward(childReqs, outputs);

        return {std::move(branchSlots), std::move(branch)};
    };

    // Build an anchor branch of the union and add a constant filter on top of it, so that it would
    // only execute on an initial collection scan, that is, when resumeRecordId is not available
    // yet.
    auto&& [anchorBranchSlots, anchorBranch] = makeUnionBranch(false);
    anchorBranch = sbe::makeS<sbe::FilterStage<true>>(
        std::move(anchorBranch),
        makeNot(makeFunction("exists"_sd, sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    // Build a resume branch of the union and add a constant filter on op of it, so that it would
    // only execute when we resume a collection scan from the resumeRecordId.
    auto&& [resumeBranchSlots, resumeBranch] = makeUnionBranch(true);
    resumeBranch = sbe::makeS<sbe::FilterStage<true>>(
        sbe::makeS<sbe::LimitSkipStage>(std::move(resumeBranch), boost::none, 1, root->nodeId()),
        sbe::makeE<sbe::EFunction>("exists"_sd,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    invariant(anchorBranchSlots.size() == resumeBranchSlots.size());

    // A vector of the output slots for each union branch.
    auto branchSlots = makeVector<sbe::value::SlotVector>(std::move(anchorBranchSlots),
                                                          std::move(resumeBranchSlots));

    PlanStageSlots outputs(reqs, &_slotIdGenerator);
    auto unionOutputSlots = getSlotsToForward(reqs, outputs);

    // Branch output slots become the input slots to the union.
    auto unionStage =
        sbe::makeS<sbe::UnionStage>(sbe::makeSs(std::move(anchorBranch), std::move(resumeBranch)),
                                    branchSlots,
                                    unionOutputSlots,
                                    root->nodeId());

    return {std::move(unionStage), std::move(outputs)};
}

namespace {
/**
 * Given an SBE subtree 'childStage' which computes the shard key and puts it into the given
 * 'shardKeySlot', augments the SBE plan to actually perform shard filtering. Namely, a FilterStage
 * is added at the root of the tree whose filter expression uses 'shardFiltererSlot' to determine
 * whether the shard key value in 'shardKeySlot' belongs to an owned range or not.
 */
auto buildShardFilterGivenShardKeySlot(sbe::value::SlotId shardKeySlot,
                                       std::unique_ptr<sbe::PlanStage> childStage,
                                       sbe::value::SlotId shardFiltererSlot,
                                       PlanNodeId nodeId) {
    auto shardFilterFn = makeFunction("shardFilter",
                                      sbe::makeE<sbe::EVariable>(shardFiltererSlot),
                                      sbe::makeE<sbe::EVariable>(shardKeySlot));

    return sbe::makeS<sbe::FilterStage<false>>(
        std::move(childStage), std::move(shardFilterFn), nodeId);
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildShardFilterCovered(const QuerySolutionNode* root,
                                               const PlanStageReqs& reqs) {
    // Constructs an optimized SBE plan for 'filterNode' in the case that the fields of the
    // 'shardKeyPattern' are provided by 'child'. In this case, the SBE tree for 'child' will
    // fill out slots for the necessary components of the index key. These slots can be read
    // directly in order to determine the shard key that should be passed to the
    // 'shardFiltererSlot'.
    const auto filterNode = static_cast<const ShardingFilterNode*>(root);
    auto child = filterNode->children[0].get();
    tassert(6023416,
            "buildShardFilterCovered() expects ixscan below shard filter",
            child->getType() == STAGE_IXSCAN || child->getType() == STAGE_VIRTUAL_SCAN);

    // Extract the child's key pattern.
    BSONObj indexKeyPattern = child->getType() == STAGE_IXSCAN
        ? static_cast<const IndexScanNode*>(child)->index.keyPattern
        : static_cast<const VirtualScanNode*>(child)->indexKeyPattern;

    auto childReqs = reqs.copy();

    // If we're sharded make sure that we don't return data that isn't owned by the shard. This
    // situation can occur when pending documents from in-progress migrations are inserted and when
    // there are orphaned documents from aborted migrations. To check if the document is owned by
    // the shard, we need to own a 'ShardFilterer', and extract the document's shard key as a
    // BSONObj.
    auto shardKeyPattern = _collections.getMainCollection().getShardKeyPattern();
    // We register the "shardFilterer" slot but we don't construct the ShardFilterer here, because
    // once constructed the ShardFilterer will prevent orphaned documents from being deleted. We
    // will construct the ShardFilterer later while preparing the SBE tree for execution.
    auto shardFiltererSlot = _data.env->registerSlot(
        "shardFilterer"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);

    for (auto&& shardKeyElt : shardKeyPattern) {
        childReqs.set(std::make_pair(PlanStageSlots::kKey, shardKeyElt.fieldNameStringData()));
    }

    auto [stage, outputs] = build(child, childReqs);

    // Maps from key name to a bool that indicates whether the key is hashed.
    StringDataMap<bool> indexKeyPatternMap;
    for (auto&& ixPatternElt : indexKeyPattern) {
        indexKeyPatternMap.emplace(ixPatternElt.fieldNameStringData(),
                                   ShardKeyPattern::isHashedPatternEl(ixPatternElt));
    }

    // Build a project stage to deal with hashed shard keys. This step *could* be skipped if we're
    // dealing with non-hashed sharding, but it's done this way for sake of simplicity.
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;
    sbe::value::SlotVector fieldSlots;
    std::vector<std::string> projectFields;
    for (auto&& shardKeyPatternElt : shardKeyPattern) {
        auto it = indexKeyPatternMap.find(shardKeyPatternElt.fieldNameStringData());
        tassert(5562303, "Could not find element", it != indexKeyPatternMap.end());
        const auto ixKeyEltHashed = it->second;
        const auto slotId = outputs.get(
            std::make_pair(PlanStageSlots::kKey, shardKeyPatternElt.fieldNameStringData()));

        // Get the value stored in the index for this component of the shard key. We may have to
        // hash it.
        auto elem = makeVariable(slotId);

        // Handle the case where the index key or shard key is hashed.
        const bool shardKeyEltHashed = ShardKeyPattern::isHashedPatternEl(shardKeyPatternElt);
        if (ixKeyEltHashed) {
            // If the index stores hashed data, then we know the shard key field is hashed as
            // well. Nothing to do here. We can apply shard filtering with no other changes.
            tassert(6023421,
                    "Index key is hashed, expected corresponding shard key to be hashed",
                    shardKeyEltHashed);
        } else if (shardKeyEltHashed) {
            // The shard key field is hashed but the index stores unhashed data. We must apply
            // the hash function before passing this off to the shard filter.
            elem = makeFunction("shardHash"_sd, std::move(elem));
        }

        fieldSlots.push_back(_slotIdGenerator.generate());
        projectFields.push_back(shardKeyPatternElt.fieldName());
        projections.emplace(fieldSlots.back(), std::move(elem));
    }

    auto projectStage = sbe::makeS<sbe::ProjectStage>(
        std::move(stage), std::move(projections), filterNode->nodeId());

    auto shardKeySlot = _slotIdGenerator.generate();
    auto mkObjStage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(projectStage),
                                                        shardKeySlot,
                                                        boost::none,
                                                        boost::none,
                                                        std::vector<std::string>{},
                                                        std::move(projectFields),
                                                        fieldSlots,
                                                        true,
                                                        false,
                                                        filterNode->nodeId());

    auto filterStage = buildShardFilterGivenShardKeySlot(
        shardKeySlot, std::move(mkObjStage), shardFiltererSlot, filterNode->nodeId());

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(filterStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildShardFilter(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto child = root->children[0].get();
    bool childIsIndexScan = child->getType() == STAGE_IXSCAN ||
        (child->getType() == STAGE_VIRTUAL_SCAN &&
         !static_cast<const VirtualScanNode*>(child)->indexKeyPattern.isEmpty());

    // If we're not required to fill out the 'kResult' slot, then instead we can request a slot from
    // the child for each of the fields which constitute the shard key. This allows us to avoid
    // materializing an intermediate object for plans where shard filtering can be performed based
    // on the contents of index keys.
    //
    // We only apply this optimization in the special case that the child QSN is an IXSCAN, since in
    // this case we can request exactly the fields we need according to their position in the index
    // key pattern.
    if (!reqs.has(kResult) && childIsIndexScan) {
        return buildShardFilterCovered(root, reqs);
    }

    auto childReqs = reqs.copy().set(kResult);

    // If we're sharded make sure that we don't return data that isn't owned by the shard. This
    // situation can occur when pending documents from in-progress migrations are inserted and when
    // there are orphaned documents from aborted migrations. To check if the document is owned by
    // the shard, we need to own a 'ShardFilterer', and extract the document's shard key as a
    // BSONObj.
    auto shardKeyPattern = _collections.getMainCollection().getShardKeyPattern();
    // We register the "shardFilterer" slot but we don't construct the ShardFilterer here, because
    // once constructed the ShardFilterer will prevent orphaned documents from being deleted. We
    // will construct the ShardFilterer later while preparing the SBE tree for execution.
    auto shardFiltererSlot = _data.env->registerSlot(
        "shardFilterer"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);

    auto [stage, outputs] = build(child, childReqs);

    // Build an expression to extract the shard key from the document based on the shard key
    // pattern. To do this, we iterate over the shard key pattern parts and build nested 'getField'
    // expressions. This will handle single-element paths, and dotted paths for each shard key part.
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;
    sbe::value::SlotVector fieldSlots;
    std::vector<std::string> projectFields;
    std::unique_ptr<sbe::EExpression> bindShardKeyPart;

    for (auto&& keyPatternElem : shardKeyPattern) {
        auto fieldRef = sbe::MatchPath{keyPatternElem.fieldNameStringData()};
        fieldSlots.push_back(_slotIdGenerator.generate());
        projectFields.push_back(fieldRef.dottedField().toString());

        auto currentFieldSlot = sbe::makeE<sbe::EVariable>(outputs.get(kResult));
        auto shardKeyBinding =
            generateShardKeyBinding(fieldRef, _frameIdGenerator, std::move(currentFieldSlot), 0);

        // If this is a hashed shard key then compute the hash value.
        if (ShardKeyPattern::isHashedPatternEl(keyPatternElem)) {
            shardKeyBinding = makeFunction("shardHash"_sd, std::move(shardKeyBinding));
        }

        projections.emplace(fieldSlots.back(), std::move(shardKeyBinding));
    }

    auto shardKeySlot{_slotIdGenerator.generate()};

    // Build an object which will hold a flattened shard key from the projections above.
    auto shardKeyObjStage = sbe::makeS<sbe::MakeBsonObjStage>(
        sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projections), root->nodeId()),
        shardKeySlot,
        boost::none,
        boost::none,
        std::vector<std::string>{},
        std::move(projectFields),
        fieldSlots,
        true,
        false,
        root->nodeId());

    // Build a project stage that checks if any of the fieldSlots for the shard key parts are an
    // Array which is represented by Nothing.
    invariant(fieldSlots.size() > 0);
    auto arrayChecks = makeNot(sbe::makeE<sbe::EFunction>(
        "exists", sbe::makeEs(sbe::makeE<sbe::EVariable>(fieldSlots[0]))));
    for (size_t ind = 1; ind < fieldSlots.size(); ++ind) {
        arrayChecks = makeBinaryOp(
            sbe::EPrimBinary::Op::logicOr,
            std::move(arrayChecks),
            makeNot(makeFunction("exists", sbe::makeE<sbe::EVariable>(fieldSlots[ind]))));
    }
    arrayChecks = sbe::makeE<sbe::EIf>(std::move(arrayChecks),
                                       sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0),
                                       sbe::makeE<sbe::EVariable>(shardKeySlot));

    auto finalShardKeySlot{_slotIdGenerator.generate()};

    auto finalShardKeyObjStage = makeProjectStage(
        std::move(shardKeyObjStage), root->nodeId(), finalShardKeySlot, std::move(arrayChecks));

    return {
        buildShardFilterGivenShardKeySlot(
            finalShardKeySlot, std::move(finalShardKeyObjStage), shardFiltererSlot, root->nodeId()),
        std::move(outputs)};
}

const CollectionPtr& SlotBasedStageBuilder::getCurrentCollection(const PlanStageReqs& reqs) const {
    return _collections.lookupCollection(reqs.getTargetNamespace());
}

// Returns a non-null pointer to the root of a plan tree, or a non-OK status if the PlanStage tree
// could not be constructed.
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::build(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    static const stdx::unordered_map<
        StageType,
        std::function<std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>(
            SlotBasedStageBuilder&, const QuerySolutionNode* root, const PlanStageReqs& reqs)>>
        kStageBuilders = {
            {STAGE_COLLSCAN, &SlotBasedStageBuilder::buildCollScan},
            {STAGE_VIRTUAL_SCAN, &SlotBasedStageBuilder::buildVirtualScan},
            {STAGE_IXSCAN, &SlotBasedStageBuilder::buildIndexScan},
            {STAGE_COLUMN_SCAN, &SlotBasedStageBuilder::buildColumnScan},
            {STAGE_FETCH, &SlotBasedStageBuilder::buildFetch},
            {STAGE_LIMIT, &SlotBasedStageBuilder::buildLimit},
            {STAGE_SKIP, &SlotBasedStageBuilder::buildSkip},
            {STAGE_SORT_SIMPLE, &SlotBasedStageBuilder::buildSort},
            {STAGE_SORT_DEFAULT, &SlotBasedStageBuilder::buildSort},
            {STAGE_SORT_KEY_GENERATOR, &SlotBasedStageBuilder::buildSortKeyGenerator},
            {STAGE_PROJECTION_SIMPLE, &SlotBasedStageBuilder::buildProjectionSimple},
            {STAGE_PROJECTION_DEFAULT, &SlotBasedStageBuilder::buildProjectionDefault},
            {STAGE_PROJECTION_COVERED, &SlotBasedStageBuilder::buildProjectionCovered},
            {STAGE_OR, &SlotBasedStageBuilder::buildOr},
            // In SBE TEXT_OR behaves like a regular OR. All the work to support "textScore"
            // metadata is done outside of TEXT_OR, unlike the legacy implementation.
            {STAGE_TEXT_OR, &SlotBasedStageBuilder::buildOr},
            {STAGE_TEXT_MATCH, &SlotBasedStageBuilder::buildTextMatch},
            {STAGE_RETURN_KEY, &SlotBasedStageBuilder::buildReturnKey},
            {STAGE_EOF, &SlotBasedStageBuilder::buildEof},
            {STAGE_AND_HASH, &SlotBasedStageBuilder::buildAndHash},
            {STAGE_AND_SORTED, &SlotBasedStageBuilder::buildAndSorted},
            {STAGE_SORT_MERGE, &SlotBasedStageBuilder::buildSortMerge},
            {STAGE_GROUP, &SlotBasedStageBuilder::buildGroup},
            {STAGE_EQ_LOOKUP, &SlotBasedStageBuilder::buildLookup},
            {STAGE_SHARDING_FILTER, &SlotBasedStageBuilder::buildShardFilter}};

    tassert(4822884,
            str::stream() << "Unsupported QSN in SBE stage builder: " << root->toString(),
            kStageBuilders.find(root->getType()) != kStageBuilders.end());

    // If this plan is for a tailable cursor scan, and we're not already in the process of building
    // a special union sub-tree implementing such scans, then start building a union sub-tree. Note
    // that LIMIT or SKIP stage is used as a splitting point of the two union branches, if present,
    // because we need to apply limit (or skip) only in the initial scan (in the anchor branch), and
    // the resume branch should not have it.
    switch (root->getType()) {
        case STAGE_COLLSCAN:
        case STAGE_LIMIT:
        case STAGE_SKIP:
            if (_cq.getFindCommandRequest().getTailable() &&
                !reqs.getIsBuildingUnionForTailableCollScan()) {
                auto childReqs = reqs;
                childReqs.setIsBuildingUnionForTailableCollScan(true);
                return makeUnionForTailableCollScan(root, childReqs);
            }
            [[fallthrough]];
        default:
            break;
    }

    auto [stage, slots] = std::invoke(kStageBuilders.at(root->getType()), *this, root, reqs);
    auto outputs = std::move(slots);

    auto fields = filterVector(reqs.getFields(), [&](const std::string& s) {
        return !outputs.has(std::make_pair(PlanStageSlots::kField, StringData(s)));
    });

    if (!fields.empty()) {
        tassert(6023424,
                str::stream() << "Expected build() for " << stageTypeToString(root->getType())
                              << " to either produce a kResult slot or to satisfy all kField reqs",
                outputs.has(PlanStageSlots::kResult));

        auto resultSlot = outputs.get(PlanStageSlots::kResult);
        auto [outStage, slots] = projectTopLevelFields(
            std::move(stage), fields, resultSlot, root->nodeId(), &_slotIdGenerator);
        stage = std::move(outStage);

        for (size_t i = 0; i < fields.size(); ++i) {
            outputs.set(std::make_pair(PlanStageSlots::kField, std::move(fields[i])), slots[i]);
        }
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder

/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include <memory>
#include <string>

#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_node.h"

namespace mongo::projection_executor {
/**
 * A node used to define the parsed structure of an exclusion projection. Each ExclusionNode
 * represents one 'level' of the parsed specification. The root ExclusionNode represents all top
 * level exclusions, with any child ExclusionNodes representing dotted or nested exclusions.
 */
class ExclusionNode final : public ProjectionNode {
public:
    ExclusionNode(ProjectionPolicies policies, std::string pathToNode = "")
        : ProjectionNode(policies, std::move(pathToNode)) {}

    ExclusionNode* addOrGetChild(const std::string& field) {
        return static_cast<ExclusionNode*>(ProjectionNode::addOrGetChild(field));
    }

    void reportDependencies(DepsTracker* deps) const final {
        // We have no dependencies on specific fields, since we only know the fields to be removed.
        // We may have expression dependencies though, as $meta expression can be used with
        // exclusion.
        for (auto&& expressionPair : _expressions) {
            expressionPair.second->addDependencies(deps);
        }

        for (auto&& childPair : _children) {
            childPair.second->reportDependencies(deps);
        }
    }

protected:
    std::unique_ptr<ProjectionNode> makeChild(std::string fieldName) const final {
        return std::make_unique<ExclusionNode>(
            _policies, FieldPath::getFullyQualifiedPath(_pathToNode, fieldName));
    }
    Document initializeOutputDocument(const Document& inputDoc) const final {
        return inputDoc;
    }
    Value applyLeafProjectionToValue(const Value& value) const final {
        return Value();
    }
    Value transformSkippedValueForOutput(const Value& value) const final {
        return value;
    }
};

/**
 * A ExclusionProjectionExecutor represents an execution tree for an exclusion projection.
 *
 * This class is mostly a wrapper around an ExclusionNode tree and defers most execution logic to
 * the underlying tree.
 */
class ExclusionProjectionExecutor : public ProjectionExecutor {
public:
    ExclusionProjectionExecutor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                ProjectionPolicies policies)
        : ProjectionExecutor(expCtx, policies), _root(new ExclusionNode(_policies)) {}

    TransformerType getType() const final {
        return TransformerType::kExclusionProjection;
    }

    const ExclusionNode* getRoot() const {
        return _root.get();
    }

    ExclusionNode* getRoot() {
        return _root.get();
    }

    Document serializeTransformation(
        boost::optional<ExplainOptions::Verbosity> explain) const final {
        return _root->serialize(explain);
    }

    /**
     * Exclude the fields specified.
     */
    Document applyProjection(const Document& inputDoc) const final {
        return _root->applyToDocument(inputDoc);
    }

    DepsTracker::State addDependencies(DepsTracker* deps) const final {
        _root->reportDependencies(deps);
        if (_rootReplacementExpression) {
            _rootReplacementExpression->addDependencies(deps);
        }
        return DepsTracker::State::SEE_NEXT;
    }

    DocumentSource::GetModPathsReturn getModifiedPaths() const final {
        // A root-replacement expression can replace the entire root document, so all paths are
        // considered as modified.
        if (_rootReplacementExpression) {
            return {DocumentSource::GetModPathsReturn::Type::kAllPaths, {}, {}};
        }

        std::set<std::string> modifiedPaths;
        _root->reportProjectedPaths(&modifiedPaths);
        return {DocumentSource::GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths), {}};
    }

private:
    // The ExclusionNode tree does most of the execution work once constructed.
    std::unique_ptr<ExclusionNode> _root;
};
}  // namespace mongo::projection_executor

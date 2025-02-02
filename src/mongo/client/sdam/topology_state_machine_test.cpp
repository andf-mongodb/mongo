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
#include "mongo/client/sdam/topology_state_machine.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/client/sdam/sdam_test_base.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/server_description_builder.h"
#include "mongo/client/sdam/topology_description.h"

namespace mongo::sdam {
class TopologyStateMachineTestFixture : public SdamTestFixture {
protected:
    static inline const auto kReplicaSetName = "replica_set";
    static inline const auto kLocalServer = "localhost:123";
    static inline const auto kLocalServer2 = "localhost:456";

    static inline const auto kTwoSeedConfig =
        SdamConfiguration(std::vector<ServerAddress>{kLocalServer, kLocalServer2},
                          TopologyType::kUnknown,
                          mongo::Milliseconds(500));
    static inline const auto kTwoSeedReplicaSetNoPrimaryConfig =
        SdamConfiguration(std::vector<ServerAddress>{kLocalServer, kLocalServer2},
                          TopologyType::kReplicaSetNoPrimary,
                          mongo::Milliseconds(500),
                          std::string("setName"));
    static inline const auto kSingleConfig =
        SdamConfiguration(std::vector<ServerAddress>{kLocalServer}, TopologyType::kSingle);

    // Given we in 'starting' state with initial config 'initialConfig'. We receive a
    // ServerDescription with type 'incoming', and expected the ending topology state to be
    // 'ending'.
    struct TopologyTypeTestCase {
        SdamConfiguration initialConfig;
        TopologyType starting;
        ServerType incoming;
        TopologyType ending;
    };

    // This function sets up the test scenario defined by the given TopologyTypeTestCase. It
    // simulates receiving a ServerDescription, and asserts that the final topology type is in the
    // correct state.
    void assertTopologyTypeTestCase(TopologyTypeTestCase testCase) {
        TopologyStateMachine stateMachine(testCase.initialConfig);

        // setup the initial state
        TopologyDescription topologyDescription(testCase.initialConfig);
        topologyDescription.setType(testCase.starting);

        // create new ServerDescription and
        auto serverDescriptionBuilder =
            ServerDescriptionBuilder().withType(testCase.incoming).withAddress(kLocalServer);

        // update the known hosts in the ServerDescription
        if (testCase.initialConfig.getSeedList()) {
            for (auto address : *testCase.initialConfig.getSeedList()) {
                serverDescriptionBuilder.withHost(address);
            }
        }

        // set the primary if we are creating one
        if (testCase.incoming == ServerType::kRSPrimary) {
            serverDescriptionBuilder.withPrimary(kLocalServer);
        }

        // set the replica set name if appropriate
        const std::vector<ServerType>& replicaSetServerTypes = std::vector<ServerType>{
            ServerType::kRSOther, ServerType::kRSSecondary, ServerType::kRSArbiter};
        if (std::find(replicaSetServerTypes.begin(),
                      replicaSetServerTypes.end(),
                      testCase.incoming) != replicaSetServerTypes.end()) {
            serverDescriptionBuilder.withSetName(kReplicaSetName);
        }

        const auto serverDescription = serverDescriptionBuilder.instance();

        // simulate the ServerDescription being received
        stateMachine.onServerDescription(topologyDescription, serverDescription);

        ASSERT_EQUALS(topologyDescription.getType(), testCase.ending);
    }

    std::vector<ServerType> allServerTypesExceptPrimary() {
        auto allExceptPrimary = allServerTypes();
        allExceptPrimary.erase(
            std::remove_if(allExceptPrimary.begin(),
                           allExceptPrimary.end(),
                           [](const ServerType t) { return t == ServerType::kRSPrimary; }),
            allExceptPrimary.end());
        return allExceptPrimary;
    }
};

TEST_F(TopologyStateMachineTestFixture, ShouldInstallServerDescriptionInSingleTopology) {
    TopologyStateMachine stateMachine(kSingleConfig);
    TopologyDescription topologyDescription(kSingleConfig);

    auto updatedMeAddress = "foo:1234";
    auto serverDescription = ServerDescriptionBuilder()
                                 .withAddress(kLocalServer)
                                 .withMe(updatedMeAddress)
                                 .withType(ServerType::kStandalone)
                                 .instance();

    stateMachine.onServerDescription(topologyDescription, serverDescription);
    ASSERT_EQUALS(static_cast<size_t>(1), topologyDescription.getServers().size());

    auto result = topologyDescription.findServerByAddress(kLocalServer);
    ASSERT(result);
    ASSERT_EQUALS(serverDescription, *result);
}

TEST_F(TopologyStateMachineTestFixture, ShouldRemoveServerDescriptionIfNotInHostsList) {
    const auto primary = (*kTwoSeedConfig.getSeedList()).front();
    const auto expectedRemovedServer = (*kTwoSeedConfig.getSeedList()).back();

    TopologyStateMachine stateMachine(kTwoSeedConfig);
    TopologyDescription topologyDescription(kTwoSeedConfig);

    auto serverDescription = ServerDescriptionBuilder()
                                 .withAddress(primary)
                                 .withType(ServerType::kRSPrimary)
                                 .withPrimary(primary)
                                 .withHost(primary)
                                 .instance();

    ASSERT_EQUALS(static_cast<size_t>(2), topologyDescription.getServers().size());
    stateMachine.onServerDescription(topologyDescription, serverDescription);
    ASSERT_EQUALS(static_cast<size_t>(1), topologyDescription.getServers().size());
    ASSERT_EQUALS(serverDescription, topologyDescription.getServers().front());
}

TEST_F(TopologyStateMachineTestFixture,
       ShouldRemoveNonPrimaryServerWhenTopologyIsReplicaSetNoPrimaryAndMeDoesntMatchAddress) {
    const auto serverAddress = (*kTwoSeedReplicaSetNoPrimaryConfig.getSeedList()).front();
    const auto expectedRemainingServerAddress =
        (*kTwoSeedReplicaSetNoPrimaryConfig.getSeedList()).back();
    const auto me = std::string("foo") + serverAddress;

    TopologyStateMachine stateMachine(kTwoSeedReplicaSetNoPrimaryConfig);
    TopologyDescription topologyDescription(kTwoSeedReplicaSetNoPrimaryConfig);

    auto serverDescription = ServerDescriptionBuilder()
                                 .withAddress(serverAddress)
                                 .withMe(me)
                                 .withType(ServerType::kRSSecondary)
                                 .instance();

    ASSERT_EQUALS(static_cast<size_t>(2), topologyDescription.getServers().size());
    stateMachine.onServerDescription(topologyDescription, serverDescription);
    ASSERT_EQUALS(static_cast<size_t>(1), topologyDescription.getServers().size());
    ASSERT_EQUALS(expectedRemainingServerAddress,
                  topologyDescription.getServers().front()->getAddress());
}

TEST_F(TopologyStateMachineTestFixture,
       ShouldAddServerDescriptionIfInHostsListButNotInTopologyDescription) {
    const auto primary = (*kTwoSeedConfig.getSeedList()).front();
    const auto secondary = (*kTwoSeedConfig.getSeedList()).back();
    const auto newHost = ServerAddress("newhost:123");

    TopologyStateMachine stateMachine(kTwoSeedConfig);
    TopologyDescription topologyDescription(kTwoSeedConfig);

    auto serverDescription = ServerDescriptionBuilder()
                                 .withAddress(primary)
                                 .withType(ServerType::kRSPrimary)
                                 .withPrimary(primary)
                                 .withHost(primary)
                                 .withHost(secondary)
                                 .withHost(newHost)
                                 .instance();

    ASSERT_EQUALS(static_cast<size_t>(2), topologyDescription.getServers().size());
    stateMachine.onServerDescription(topologyDescription, serverDescription);
    ASSERT_EQUALS(static_cast<size_t>(3), topologyDescription.getServers().size());

    auto newHostResult = topologyDescription.findServerByAddress(newHost);
    ASSERT(newHostResult);
    ASSERT_EQUALS(newHost, (*newHostResult)->getAddress());
    ASSERT_EQUALS(ServerType::kUnknown, (*newHostResult)->getType());
}

TEST_F(TopologyStateMachineTestFixture, ShouldSaveNewMaxSetVersion) {
    const auto primary = (*kTwoSeedConfig.getSeedList()).front();

    TopologyDescription topologyDescription(kTwoSeedConfig);
    TopologyStateMachine stateMachine(kTwoSeedConfig);

    auto serverDescription = ServerDescriptionBuilder()
                                 .withType(ServerType::kRSPrimary)
                                 .withPrimary(primary)
                                 .withMe(primary)
                                 .withAddress(primary)
                                 .withHost(primary)
                                 .withSetVersion(100)
                                 .instance();

    stateMachine.onServerDescription(topologyDescription, serverDescription);
    ASSERT_EQUALS(100, topologyDescription.getMaxSetVersion());

    auto serverDescriptionEvenBiggerSetVersion = ServerDescriptionBuilder()
                                                     .withType(ServerType::kRSPrimary)
                                                     .withPrimary(primary)
                                                     .withMe(primary)
                                                     .withAddress(primary)
                                                     .withHost(primary)
                                                     .withSetVersion(200)
                                                     .instance();

    stateMachine.onServerDescription(topologyDescription, serverDescriptionEvenBiggerSetVersion);
    ASSERT_EQUALS(200, topologyDescription.getMaxSetVersion());
}

TEST_F(TopologyStateMachineTestFixture, ShouldSaveNewMaxElectionId) {
    const auto primary = (*kTwoSeedConfig.getSeedList()).front();
    TopologyDescription topologyDescription(kTwoSeedConfig);
    TopologyStateMachine stateMachine(kTwoSeedConfig);

    const OID oidOne(std::string("000000000000000000000001"));
    const OID oidTwo(std::string("000000000000000000000002"));

    auto serverDescription = ServerDescriptionBuilder()
                                 .withType(ServerType::kRSPrimary)
                                 .withPrimary(primary)
                                 .withMe(primary)
                                 .withAddress(primary)
                                 .withHost(primary)
                                 .withSetVersion(1)
                                 .withElectionId(oidOne)
                                 .instance();

    stateMachine.onServerDescription(topologyDescription, serverDescription);
    ASSERT_EQUALS(oidOne, topologyDescription.getMaxElectionId());

    auto serverDescriptionEvenBiggerElectionId = ServerDescriptionBuilder()
                                                     .withType(ServerType::kRSPrimary)
                                                     .withPrimary(primary)
                                                     .withMe(primary)
                                                     .withAddress(primary)
                                                     .withHost(primary)
                                                     .withSetVersion(1)
                                                     .withElectionId(oidTwo)
                                                     .instance();

    stateMachine.onServerDescription(topologyDescription, serverDescriptionEvenBiggerElectionId);
    ASSERT_EQUALS(oidTwo, topologyDescription.getMaxElectionId());
}

// The following two tests (ShouldNotUpdateToplogyType, ShouldUpdateToCorrectToplogyType) assert
// that the topology type is correct given an initial state and a ServerType. Together, they
// cover all the cases specified in the SDAM spec here:
// https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#topologytype-table

TEST_F(TopologyStateMachineTestFixture, ShouldNotUpdateToplogyType) {
    using T = TopologyTypeTestCase;

    // test cases that should not change TopologyType
    std::vector<TopologyTypeTestCase> testCases{
        T{kTwoSeedConfig, TopologyType::kUnknown, ServerType::kUnknown, TopologyType::kUnknown},
        T{kTwoSeedConfig, TopologyType::kUnknown, ServerType::kStandalone, TopologyType::kUnknown},
        T{kTwoSeedConfig, TopologyType::kUnknown, ServerType::kRSGhost, TopologyType::kUnknown},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetNoPrimary,
          ServerType::kUnknown,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetNoPrimary,
          ServerType::kUnknown,
          TopologyType::kReplicaSetNoPrimary},
    };
    for (auto serverType : allServerTypes()) {
        testCases.push_back(
            T{kTwoSeedConfig, TopologyType::kSharded, serverType, TopologyType::kSharded});
    }

    const auto& allExceptPrimary = allServerTypesExceptPrimary();
    for (auto serverType : allExceptPrimary) {
        testCases.push_back(T{kTwoSeedConfig,
                              TopologyType::kReplicaSetNoPrimary,
                              serverType,
                              TopologyType::kReplicaSetNoPrimary});
    }

    int count = 0;
    for (auto testCase : testCases) {
        std::cout << "case " << ++count << " starting TopologyType: " << toString(testCase.starting)
                  << "; incoming ServerType: " << toString(testCase.incoming)
                  << "; expect ending TopologyType: " << toString(testCase.ending) << std::endl;

        assertTopologyTypeTestCase(testCase);
    }
}

TEST_F(TopologyStateMachineTestFixture, ShouldUpdateToCorrectToplogyType) {
    using T = TopologyTypeTestCase;

    // test cases that should change TopologyType
    const std::vector<TopologyTypeTestCase> testCases{
        T{kTwoSeedConfig, TopologyType::kUnknown, ServerType::kMongos, TopologyType::kSharded},
        T{kTwoSeedConfig,
          TopologyType::kUnknown,
          ServerType::kRSPrimary,
          TopologyType::kReplicaSetWithPrimary},
        T{kTwoSeedConfig,
          TopologyType::kUnknown,
          ServerType::kRSSecondary,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kUnknown,
          ServerType::kRSArbiter,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kUnknown,
          ServerType::kRSOther,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetNoPrimary,
          ServerType::kRSPrimary,
          TopologyType::kReplicaSetWithPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kUnknown,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kStandalone,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kMongos,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kRSPrimary,
          TopologyType::kReplicaSetWithPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kRSSecondary,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kRSOther,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kRSArbiter,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kRSGhost,
          TopologyType::kReplicaSetNoPrimary}};

    int count = 0;
    for (auto testCase : testCases) {
        std::cout << "case " << ++count << " starting TopologyType: " << toString(testCase.starting)
                  << "; incoming ServerType: " << toString(testCase.incoming)
                  << "; expect ending TopologyType: " << toString(testCase.ending) << std::endl;

        assertTopologyTypeTestCase(testCase);
    }
}
}  // namespace mongo::sdam

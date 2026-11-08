// Fast DDS reliable publisher for the RTPS interop test.
// Sends HelloWorld samples on hello_world_topic with explicit RELIABLE QoS and a
// non-default max_blocking_time so the reliability PID is announced.
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/core/policy/QosPolicies.hpp>

#include "HelloWorldPubSubTypes.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace eprosima::fastdds::dds;

int main(int argc, char **argv)
{
    const int count = argc > 1 ? std::atoi(argv[1]) : 3;
    const int msg_len = argc > 2 ? std::atoi(argv[2]) : 16;
    const int wait_s = argc > 3 ? std::atoi(argv[3]) : 0;

    /* Optional XML participant profile. A profile is the portable way to pin
     * Fast DDS to one interface (interfaceWhiteList), which 2.x needs when the
     * host also has a loopback route to the nb peer. */
    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    if (const char *prof = std::getenv("FDDS_PROFILE")) {
        DomainParticipantFactory::get_instance()->load_profiles();
        DomainParticipantFactory::get_instance()->get_participant_qos_from_profile(prof, pqos);
    }
    DomainParticipant *participant =
        DomainParticipantFactory::get_instance()->create_participant(0, pqos);
    if (!participant) {
        std::fprintf(stderr, "participant create failed\n");
        return 1;
    }
    TypeSupport type(new HelloWorldPubSubType());
    type.register_type(participant);
    Topic *topic = participant->create_topic("hello_world_topic", type.get_type_name(), TOPIC_QOS_DEFAULT);
    Publisher *publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.reliability().max_blocking_time = Duration_t(0, 200000000);
    if (std::getenv("FDDS_TL")) {
        wqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    }
    DataWriter *writer = publisher->create_datawriter(topic, wqos);

    if (!std::getenv("FDDS_NO_WAIT")) {
        const int want = std::getenv("FDDS_MATCHED")
                             ? std::atoi(std::getenv("FDDS_MATCHED"))
                             : 1;
        for (int i = 0; i < 50; i++) {
            PublicationMatchedStatus status;
            if (writer->get_publication_matched_status(status) == RETCODE_OK &&
                status.current_count >= want) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    PublicationMatchedStatus status;
    writer->get_publication_matched_status(status);
    std::printf("matched=%d\n", (int)status.current_count);
    if (status.current_count == 0 && !std::getenv("FDDS_NO_WAIT")) {
        return 3;
    }

    HelloWorld sample;
    for (int i = 0; i < count; i++) {
        sample.index((uint32_t)(i + 1));
        sample.message(std::string((size_t)msg_len, (char)('B' + (i % 20))));
        if (writer->write(&sample, HANDLE_NIL) != RETCODE_OK) {
            std::fprintf(stderr, "write failed\n");
            return 2;
        }
        std::printf("SENT index=%d len=%d\n", i + 1, msg_len);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (wait_s > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(wait_s));
    }
    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}

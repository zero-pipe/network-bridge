// Fast DDS reliable subscriber for the RTPS interop test.
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/core/policy/QosPolicies.hpp>

#include "HelloWorldPubSubTypes.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace eprosima::fastdds::dds;

static std::atomic<int> g_received{0};

class Listener : public DataReaderListener {
public:
    void on_data_available(DataReader *reader) override
    {
        HelloWorld sample;
        SampleInfo info;
        while (reader->take_next_sample(&sample, &info) == RETCODE_OK) {
            if (info.valid_data) {
                std::printf("RECV index=%u len=%zu msg='%.*s'\n",
                            sample.index(),
                            sample.message().size(),
                            (int)(sample.message().size() > 32 ? 32 : sample.message().size()),
                            sample.message().c_str());
                std::fflush(stdout);
                g_received++;
            }
        }
    }
};

int main(int argc, char **argv)
{
    const int expect = argc > 1 ? std::atoi(argv[1]) : 3;
    const int timeout_s = argc > 2 ? std::atoi(argv[2]) : 20;

    /* Optional XML participant profile; see fdds_qos_pub.cpp. */
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
    Subscriber *subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);

    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    rqos.reliability().max_blocking_time = Duration_t(0, 200000000);
    if (std::getenv("FDDS_SUB_TL")) {
        rqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    }
    Listener listener;
    DataReader *reader = subscriber->create_datareader(topic, rqos, &listener);

    std::printf("READY subscriber\n");
    std::fflush(stdout);
    for (int i = 0; i < timeout_s * 10 && g_received.load() < expect; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("RESULT received=%d\n", g_received.load());
    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return g_received.load() > 0 ? 0 : 2;
}

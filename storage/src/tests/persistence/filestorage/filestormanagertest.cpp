// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <tests/common/testhelper.h>
#include <tests/common/dummystoragelink.h>
#include <tests/common/teststorageapp.h>
#include <tests/persistence/filestorage/forwardingmessagesender.h>
#include <vespa/document/repo/documenttyperepo.h>
#include <vespa/document/test/make_document_bucket.h>
#include <vespa/storage/storageserver/statemanager.h>
#include <vespa/storage/bucketdb/bucketmanager.h>
#include <vespa/storage/persistence/persistencethread.h>
#include <vespa/storage/persistence/filestorage/filestormanager.h>
#include <vespa/storage/persistence/filestorage/modifiedbucketchecker.h>
#include <vespa/document/update/assignvalueupdate.h>
#include <vespa/document/update/documentupdate.h>
#include <vespa/document/select/parser.h>
#include <vespa/vdslib/state/random.h>
#include <vespa/storageapi/message/bucketsplitting.h>
#include <vespa/persistence/dummyimpl/dummypersistence.h>
#include <vespa/persistence/spi/test.h>
#include <vespa/config/common/exceptions.h>
#include <vespa/fastos/file.h>
#include <vespa/vespalib/gtest/gtest.h>
#include <atomic>

#include <vespa/log/log.h>
LOG_SETUP(".filestormanagertest");

using std::unique_ptr;
using document::Document;
using namespace storage::api;
using storage::spi::test::makeSpiBucket;
using document::test::makeDocumentBucket;
using namespace ::testing;

#define ASSERT_SINGLE_REPLY(replytype, reply, link, time) \
reply = nullptr; \
try{ \
    link.waitForMessages(1, time); \
    ASSERT_EQ(1, link.getNumReplies()); \
    reply = dynamic_cast<replytype*>(link.getReply(0).get()); \
    if (reply == nullptr) { \
        FAIL() << "Got reply of unexpected type: " \
               << link.getReply(0)->getType().toString(); \
    } \
} catch (vespalib::Exception& e) { \
    reply = nullptr; \
    FAIL() << "Failed to find single reply in time"; \
}

namespace storage {

namespace {

spi::LoadType defaultLoadType(0, "default");

struct TestFileStorComponents;

}

struct FileStorManagerTest : Test{
    enum {LONG_WAITTIME=60};
    unique_ptr<TestServiceLayerApp> _node;
    std::unique_ptr<vdstestlib::DirConfig> config;
    std::unique_ptr<vdstestlib::DirConfig> config2;
    std::unique_ptr<vdstestlib::DirConfig> smallConfig;
    const uint32_t _waitTime;
    const document::DocumentType* _testdoctype1;

    FileStorManagerTest() : _node(), _waitTime(LONG_WAITTIME) {}

    void SetUp() override;
    void TearDown() override;

    void createBucket(document::BucketId bid, uint16_t disk)
    {
        spi::Context context(defaultLoadType, spi::Priority(0), spi::Trace::TraceLevel(0));
        _node->getPersistenceProvider().createBucket(makeSpiBucket(bid, spi::PartitionId(disk)), context);

        StorBucketDatabase::WrappedEntry entry(
                _node->getStorageBucketDatabase().get(bid, "foo", StorBucketDatabase::CREATE_IF_NONEXISTING));
        entry->disk = disk;
        entry->info = api::BucketInfo(0, 0, 0, 0, 0, true, false);
        entry.write();
    }

    document::Document::UP createDocument(const std::string& content, const std::string& id)
    {
        return _node->getTestDocMan().createDocument(content, id);
    }

    bool ownsBucket(uint16_t distributorIndex,
                    const document::BucketId& bucket) const
    {
        auto clusterStateBundle = _node->getStateUpdater().getClusterStateBundle();
        const auto &clusterState = *clusterStateBundle->getBaselineClusterState();
        uint16_t distributor(
                _node->getDistribution()->getIdealDistributorNode(
                        clusterState, bucket));
        return distributor == distributorIndex;
    }
    
    document::BucketId getFirstBucketNotOwnedByDistributor(uint16_t distributor) {
        for (int i = 0; i < 1000; ++i) {
            if (!ownsBucket(distributor, document::BucketId(16, i))) {
                return document::BucketId(16, i);
            }
        }
        return document::BucketId(0);
    }

    spi::dummy::DummyPersistence& getDummyPersistence() {
        return static_cast<spi::dummy::DummyPersistence&>
            (_node->getPersistenceProvider());
    }

    void setClusterState(const std::string& state) {
        _node->getStateUpdater().setClusterState(
                lib::ClusterState::CSP(
                        new lib::ClusterState(state)));
    }

    void setupDisks(uint32_t diskCount) {
        std::string rootOfRoot = "filestormanagertest";
        config.reset(new vdstestlib::DirConfig(getStandardConfig(true, rootOfRoot)));

        config2.reset(new vdstestlib::DirConfig(*config));
        config2->getConfig("stor-server").set("root_folder", rootOfRoot + "-vdsroot.2");
        config2->getConfig("stor-devices").set("root_folder", rootOfRoot + "-vdsroot.2");
        config2->getConfig("stor-server").set("node_index", "1");

        smallConfig.reset(new vdstestlib::DirConfig(*config));
        vdstestlib::DirConfig::Config& c(
                smallConfig->getConfig("stor-filestor", true));
        c.set("initial_index_read", "128");
        c.set("use_direct_io", "false");
        c.set("maximum_gap_to_read_through", "64");

        assert(system(vespalib::make_string("rm -rf %s", getRootFolder(*config).c_str()).c_str()) == 0);
        assert(system(vespalib::make_string("rm -rf %s", getRootFolder(*config2).c_str()).c_str()) == 0);
        assert(system(vespalib::make_string("mkdir -p %s/disks/d0", getRootFolder(*config).c_str()).c_str()) == 0);
        assert(system(vespalib::make_string("mkdir -p %s/disks/d0", getRootFolder(*config2).c_str()).c_str()) == 0);
        try {
            _node.reset(new TestServiceLayerApp(DiskCount(diskCount), NodeIndex(0),
                                                config->getConfigId()));
            _node->setupDummyPersistence();
        } catch (config::InvalidConfigException& e) {
            fprintf(stderr, "%s\n", e.what());
        }
        _testdoctype1 = _node->getTypeRepo()->getDocumentType("testdoctype1");
    }

    void putDoc(DummyStorageLink& top,
                FileStorHandler& filestorHandler,
                const document::BucketId& bucket,
                uint32_t docNum);

    template <typename Metric>
    void assert_request_size_set(TestFileStorComponents& c,
                                 std::shared_ptr<api::StorageMessage> cmd,
                                 const Metric& metric);

    auto& thread_metrics_of(FileStorManager& manager) {
        return manager._metrics->disks[0]->threads[0];
    }
};

std::string findFile(const std::string& path, const std::string& file) {
    FastOS_DirectoryScan dirScan(path.c_str());
    while (dirScan.ReadNext()) {
        if (dirScan.GetName()[0] == '.') {
            // Ignore current and parent dir.. Ignores hidden files too, but
            // that doesn't matter as we're not trying to find them.
            continue;
        }
        std::string filename(dirScan.GetName());
        if (dirScan.IsDirectory()) {
            std::string result = findFile(path + "/" + filename, file);
            if (result != "") {
                return result;
            }
        }
        if (filename == file) {
            return path + "/" + filename;
        }
    }
    return "";
}

bool fileExistsWithin(const std::string& path, const std::string& file) {
    return !(findFile(path, file) == "");
}

std::unique_ptr<DiskThread> createThread(vdstestlib::DirConfig& config,
                                       TestServiceLayerApp& node,
                                       spi::PersistenceProvider& provider,
                                       FileStorHandler& filestorHandler,
                                       FileStorThreadMetrics& metrics,
                                       uint16_t deviceIndex)
{
    (void) config;
    std::unique_ptr<DiskThread> disk;
    disk.reset(new PersistenceThread(
            node.getComponentRegister(), config.getConfigId(), provider,
            filestorHandler, metrics,
            deviceIndex));
    return disk;
}

namespace {

struct TestFileStorComponents {
    DummyStorageLink top;
    FileStorManager* manager;

    explicit TestFileStorComponents(FileStorManagerTest& test)
        : manager(new FileStorManager(test.config->getConfigId(),
                                      test._node->getPartitions(),
                                      test._node->getPersistenceProvider(),
                                      test._node->getComponentRegister()))
    {
        top.push_back(unique_ptr<StorageLink>(manager));
        top.open();
    }
};

}

void
FileStorManagerTest::SetUp()
{
    setupDisks(1);
}

void
FileStorManagerTest::TearDown()
{
    _node.reset(0);
}

TEST_F(FileStorManagerTest, header_only_put) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager =
            new FileStorManager(config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
    // Creating a document to test with
    Document::SP doc(createDocument(
                "some content", "id:crawler:testdoctype1:n=4000:foo").release());

    document::BucketId bid(16, 4000);

    createBucket(bid, 0);

    // Putting it
    {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 105);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
        EXPECT_EQ(1, reply->getBucketInfo().getDocumentCount());
    }
    doc->setValue(doc->getField("headerval"), document::IntFieldValue(42));
    // Putting it again, this time with header only
    {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 124);
        cmd->setUpdateTimestamp(105);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode::OK, reply->getResult().getResult());
    }
    // Getting it
    {
        auto cmd = std::make_shared<api::GetCommand>(makeDocumentBucket(bid), doc->getId(), "[all]");
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        std::shared_ptr<api::GetReply> reply2(
                std::dynamic_pointer_cast<api::GetReply>(
                    top.getReply(0)));
        top.reset();
        ASSERT_TRUE(reply2.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply2->getResult());
        EXPECT_EQ(doc->getId().toString(), reply2->getDocumentId().toString());
        // Ensure partial update was done, but other things are equal
        auto value = reply2->getDocument()->getValue(doc->getField("headerval"));
        ASSERT_TRUE(value.get());
        EXPECT_EQ(42, dynamic_cast<document::IntFieldValue&>(*value).getAsInt());
        reply2->getDocument()->remove("headerval");
        doc->remove("headerval");
        EXPECT_EQ(*doc, *reply2->getDocument());
    }
}

TEST_F(FileStorManagerTest, put) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager =
            new FileStorManager(config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
    // Creating a document to test with
    Document::SP doc(createDocument(
                "some content", "id:crawler:testdoctype1:n=4000:foo").release());

    document::BucketId bid(16, 4000);

    createBucket(bid, 0);

    // Putting it
    {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 105);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
        EXPECT_EQ(1, reply->getBucketInfo().getDocumentCount());
    }
}

TEST_F(FileStorManagerTest, disk_move) {
    setupDisks(2);

    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager =
            new FileStorManager(config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
    // Creating a document to test with
    Document::SP doc(createDocument(
                "some content", "id:crawler:testdoctype1:n=4000:foo").release());

    document::BucketId bid(16, 4000);

    createBucket(bid, 0);

    // Putting it
    {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 105);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
        EXPECT_EQ(1, reply->getBucketInfo().getDocumentCount());
    }

    {
        StorBucketDatabase::WrappedEntry entry(
                _node->getStorageBucketDatabase().get(bid, "foo"));

        EXPECT_EQ(0, entry->disk);
        EXPECT_EQ(
                vespalib::string(
                        "BucketInfo(crc 0x3538028e, docCount 1, totDocSize 124, "
                        "ready true, active false)"),
                entry->getBucketInfo().toString());
    }

    {
        auto cmd = std::make_shared<BucketDiskMoveCommand>(makeDocumentBucket(bid), 0, 1);

        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<BucketDiskMoveReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
        EXPECT_EQ(1, reply->getBucketInfo().getDocumentCount());
    }

    {
        StorBucketDatabase::WrappedEntry entry(
                _node->getStorageBucketDatabase().get(bid, "foo"));

        EXPECT_EQ(1, entry->disk);
        EXPECT_EQ(
                vespalib::string(
                        "BucketInfo(crc 0x3538028e, docCount 1, totDocSize 124, "
                        "ready true, active false)"),
                entry->getBucketInfo().toString());
    }
}

TEST_F(FileStorManagerTest, state_change) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager =
            new FileStorManager(config->getConfigId(), _node->getPartitions(),
                                _node->getPersistenceProvider(),
                                _node->getComponentRegister())));
    top.open();

    setClusterState("storage:3 distributor:3");
    EXPECT_TRUE(getDummyPersistence().getClusterState().nodeUp());

    setClusterState("storage:3 .0.s:d distributor:3");
    EXPECT_FALSE(getDummyPersistence().getClusterState().nodeUp());
}

TEST_F(FileStorManagerTest, repair_notifies_distributor_on_change) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager =
            new FileStorManager(config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    setClusterState("storage:1 distributor:1");
    top.open();

    createBucket(document::BucketId(16, 1), 0);

    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);

    // Creating a document to test with

    for (uint32_t i = 0; i < 3; ++i) {
        document::DocumentId docId(vespalib::make_string("id:ns:testdoctype1:n=1:%d", i));
        auto doc = std::make_shared<Document>(*_testdoctype1, docId);
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(document::BucketId(16, 1)), doc, i + 1);
        cmd->setAddress(address);
        top.sendDown(cmd);
    }

    top.waitForMessages(3, _waitTime);
    top.reset();

    getDummyPersistence().simulateMaintenanceFailure();

    auto cmd = std::make_shared<RepairBucketCommand>(makeDocumentBucket(document::BucketId(16, 1)), 0);
    top.sendDown(cmd);

    top.waitForMessages(2, _waitTime);

    EXPECT_EQ(
            std::string("NotifyBucketChangeCommand(BucketId(0x4000000000000001), "
                        "BucketInfo(crc 0xa14e7e3f, docCount 2, totDocSize 174, "
                        "ready true, active false))"), top.getReply(0)->toString());

    top.close();
}

TEST_F(FileStorManagerTest, flush) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager = new FileStorManager(
                config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
    // Creating a document to test with

    document::DocumentId docId("doc:crawler:http://www.ntnu.no/");
    auto doc = std::make_shared<Document>(*_testdoctype1, docId);
    document::BucketId bid(4000);

    static const uint32_t msgCount = 10;

    // Generating many put commands
    std::vector<std::shared_ptr<api::StorageCommand> > _commands;
    for (uint32_t i=0; i<msgCount; ++i) {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, i+1);
        cmd->setAddress(address);
        _commands.push_back(cmd);
    }
    for (uint32_t i=0; i<msgCount; ++i) {
        top.sendDown(_commands[i]);
    }
    top.close();
    top.flush();
    EXPECT_EQ(msgCount, top.getNumReplies());
}

TEST_F(FileStorManagerTest, handler_priority) {
    // Setup a filestorthread to test
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(
                          dummyManager = new DummyStorageLink));
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);
    // Since we fake time with small numbers, we need to make sure we dont
    // compact them away, as they will seem to be from 1970

    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(), 1, 1);

    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    filestorHandler.setGetNextMessageTimeout(50);
    uint32_t stripeId = filestorHandler.getNextStripeId(0);
    ASSERT_EQ(0u, stripeId);

    std::string content("Here is some content which is in all documents");
    std::ostringstream uri;

    Document::SP doc(createDocument(content, "id:footype:testdoctype1:n=1234:bar").release());

    document::BucketIdFactory factory;
    document::BucketId bucket(16, factory.getBucketId(doc->getId()).getRawId());

    // Populate bucket with the given data
    for (uint32_t i = 1; i < 6; i++) {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bucket), doc, 100);
        auto address = std::make_shared<api::StorageMessageAddress>("storage", lib::NodeType::STORAGE, 3);
        cmd->setAddress(*address);
        cmd->setPriority(i * 15);
        filestorHandler.schedule(cmd, 0);
    }

    ASSERT_EQ(15, filestorHandler.getNextMessage(0, stripeId).second->getPriority());
    ASSERT_EQ(30, filestorHandler.getNextMessage(0, stripeId).second->getPriority());
    ASSERT_EQ(45, filestorHandler.getNextMessage(0, stripeId).second->getPriority());
    ASSERT_EQ(60, filestorHandler.getNextMessage(0, stripeId).second->getPriority());
    ASSERT_EQ(75, filestorHandler.getNextMessage(0, stripeId).second->getPriority());
}

class MessagePusherThread : public document::Runnable {
public:
    FileStorHandler& _handler;
    Document::SP _doc;
    std::atomic<bool> _done;
    std::atomic<bool> _threadDone;

    MessagePusherThread(FileStorHandler& handler, Document::SP doc);
    ~MessagePusherThread() override;

    void run() override {
        while (!_done) {
            document::BucketIdFactory factory;
            document::BucketId bucket(16, factory.getBucketId(_doc->getId()).getRawId());

            auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bucket), _doc, 100);
            _handler.schedule(cmd, 0);
            FastOS_Thread::Sleep(1);
        }

        _threadDone = true;
    }
};

MessagePusherThread::MessagePusherThread(FileStorHandler& handler, Document::SP doc)
    : _handler(handler), _doc(std::move(doc)), _done(false), _threadDone(false)
{}
MessagePusherThread::~MessagePusherThread() = default;

class MessageFetchingThread : public document::Runnable {
public:
    const uint32_t _threadId;
    FileStorHandler& _handler;
    std::atomic<uint32_t> _config;
    std::atomic<uint32_t> _fetchedCount;
    std::atomic<bool> _done;
    std::atomic<bool> _failed;
    std::atomic<bool> _threadDone;

    explicit MessageFetchingThread(FileStorHandler& handler)
        : _threadId(handler.getNextStripeId(0)), _handler(handler), _config(0), _fetchedCount(0), _done(false),
          _failed(false), _threadDone(false)
    {}

    void run() override {
        while (!_done) {
            FileStorHandler::LockedMessage msg = _handler.getNextMessage(0, _threadId);
            if (msg.second.get()) {
                uint32_t originalConfig = _config.load();
                _fetchedCount++;
                FastOS_Thread::Sleep(5);

                if (_config.load() != originalConfig) {
                    _failed = true;
                }
            } else {
                FastOS_Thread::Sleep(1);
            }
        }

        _threadDone = true;
    };
};

TEST_F(FileStorManagerTest, handler_paused_multi_thread) {
    // Setup a filestorthread to test
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(
                          dummyManager = new DummyStorageLink));
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);
    // Since we fake time with small numbers, we need to make sure we dont
    // compact them away, as they will seem to be from 1970

    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(), 1, 1);

    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    filestorHandler.setGetNextMessageTimeout(50);

    std::string content("Here is some content which is in all documents");
    std::ostringstream uri;

    Document::SP doc(createDocument(content, "id:footype:testdoctype1:n=1234:bar").release());

    FastOS_ThreadPool pool(512 * 1024);
    MessagePusherThread pushthread(filestorHandler, doc);
    pushthread.start(pool);

    MessageFetchingThread thread(filestorHandler);
    thread.start(pool);

    for (uint32_t i = 0; i < 50; ++i) {
        FastOS_Thread::Sleep(2);
        ResumeGuard guard = filestorHandler.pause();
        thread._config.fetch_add(1);
        uint32_t count = thread._fetchedCount;
        ASSERT_EQ(count, thread._fetchedCount.load());
    }

    pushthread._done = true;
    thread._done = true;
    ASSERT_FALSE(thread._failed);

    while (!pushthread._threadDone || !thread._threadDone) {
        FastOS_Thread::Sleep(1);
    }
}

TEST_F(FileStorManagerTest, handler_pause) {
    // Setup a filestorthread to test
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(dummyManager = new DummyStorageLink));
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);
    // Since we fake time with small numbers, we need to make sure we dont
    // compact them away, as they will seem to be from 1970

    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(), 1, 1);

    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    filestorHandler.setGetNextMessageTimeout(50);
    uint32_t stripeId = filestorHandler.getNextStripeId(0);

    std::string content("Here is some content which is in all documents");
    std::ostringstream uri;

    Document::SP doc(createDocument(content, "id:footype:testdoctype1:n=1234:bar").release());

    document::BucketIdFactory factory;
    document::BucketId bucket(16, factory.getBucketId(doc->getId()).getRawId());

    // Populate bucket with the given data
    for (uint32_t i = 1; i < 6; i++) {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bucket), doc, 100);
        auto address = std::make_unique<api::StorageMessageAddress>("storage", lib::NodeType::STORAGE, 3);
        cmd->setAddress(*address);
        cmd->setPriority(i * 15);
        filestorHandler.schedule(cmd, 0);
    }

    ASSERT_EQ(15, filestorHandler.getNextMessage(0, stripeId).second->getPriority());

    {
        ResumeGuard guard = filestorHandler.pause();
        (void)guard;
        ASSERT_EQ(filestorHandler.getNextMessage(0, stripeId).second.get(), nullptr);
    }

    ASSERT_EQ(30, filestorHandler.getNextMessage(0, stripeId).second->getPriority());
}

namespace {

uint64_t getPutTime(api::StorageMessage::SP& msg)
{
    if (!msg.get()) {
        return (uint64_t)-1;
    }

    return static_cast<api::PutCommand*>(msg.get())->getTimestamp();
};

}

TEST_F(FileStorManagerTest, remap_split) {
    // Setup a filestorthread to test
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(dummyManager = new DummyStorageLink));
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);
    // Since we fake time with small numbers, we need to make sure we dont
    // compact them away, as they will seem to be from 1970

    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(), 1, 1);

    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    filestorHandler.setGetNextMessageTimeout(50);

    std::string content("Here is some content which is in all documents");

    Document::SP doc1(createDocument(content, "id:footype:testdoctype1:n=1234:bar").release());

    Document::SP doc2(createDocument(content, "id:footype:testdoctype1:n=4567:bar").release());

    document::BucketIdFactory factory;
    document::BucketId bucket1(16, 1234);
    document::BucketId bucket2(16, 4567);

    // Populate bucket with the given data
    for (uint32_t i = 1; i < 4; i++) {
        filestorHandler.schedule(std::make_shared<api::PutCommand>(makeDocumentBucket(bucket1), doc1, i), 0);
        filestorHandler.schedule(std::make_shared<api::PutCommand>(makeDocumentBucket(bucket2), doc2, i + 10), 0);
    }

    EXPECT_EQ("BucketId(0x40000000000004d2): Put(BucketId(0x40000000000004d2), id:footype:testdoctype1:n=1234:bar, timestamp 1, size 118) (priority: 127)\n"
              "BucketId(0x40000000000011d7): Put(BucketId(0x40000000000011d7), id:footype:testdoctype1:n=4567:bar, timestamp 11, size 118) (priority: 127)\n"
              "BucketId(0x40000000000004d2): Put(BucketId(0x40000000000004d2), id:footype:testdoctype1:n=1234:bar, timestamp 2, size 118) (priority: 127)\n"
              "BucketId(0x40000000000011d7): Put(BucketId(0x40000000000011d7), id:footype:testdoctype1:n=4567:bar, timestamp 12, size 118) (priority: 127)\n"
              "BucketId(0x40000000000004d2): Put(BucketId(0x40000000000004d2), id:footype:testdoctype1:n=1234:bar, timestamp 3, size 118) (priority: 127)\n"
              "BucketId(0x40000000000011d7): Put(BucketId(0x40000000000011d7), id:footype:testdoctype1:n=4567:bar, timestamp 13, size 118) (priority: 127)\n",
              filestorHandler.dumpQueue(0));

    FileStorHandler::RemapInfo a(makeDocumentBucket(document::BucketId(17, 1234)), 0);
    FileStorHandler::RemapInfo b(makeDocumentBucket(document::BucketId(17, 1234 | 1 << 16)), 0);
    filestorHandler.remapQueueAfterSplit(FileStorHandler::RemapInfo(makeDocumentBucket(bucket1), 0), a, b);

    ASSERT_TRUE(a.foundInQueue);
    ASSERT_FALSE(b.foundInQueue);

    EXPECT_EQ("BucketId(0x40000000000011d7): Put(BucketId(0x40000000000011d7), id:footype:testdoctype1:n=4567:bar, timestamp 11, size 118) (priority: 127)\n"
              "BucketId(0x40000000000011d7): Put(BucketId(0x40000000000011d7), id:footype:testdoctype1:n=4567:bar, timestamp 12, size 118) (priority: 127)\n"
              "BucketId(0x40000000000011d7): Put(BucketId(0x40000000000011d7), id:footype:testdoctype1:n=4567:bar, timestamp 13, size 118) (priority: 127)\n"
              "BucketId(0x44000000000004d2): Put(BucketId(0x44000000000004d2), id:footype:testdoctype1:n=1234:bar, timestamp 1, size 118) (priority: 127)\n"
              "BucketId(0x44000000000004d2): Put(BucketId(0x44000000000004d2), id:footype:testdoctype1:n=1234:bar, timestamp 2, size 118) (priority: 127)\n"
              "BucketId(0x44000000000004d2): Put(BucketId(0x44000000000004d2), id:footype:testdoctype1:n=1234:bar, timestamp 3, size 118) (priority: 127)\n",
              filestorHandler.dumpQueue(0));
}

TEST_F(FileStorManagerTest, handler_multi) {
    // Setup a filestorthread to test
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(
                          dummyManager = new DummyStorageLink));
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);
    // Since we fake time with small numbers, we need to make sure we dont
    // compact them away, as they will seem to be from 1970

    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(), 1, 1);

    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    filestorHandler.setGetNextMessageTimeout(50);
    uint32_t stripeId = filestorHandler.getNextStripeId(0);

    std::string content("Here is some content which is in all documents");

    Document::SP doc1(createDocument(content, "id:footype:testdoctype1:n=1234:bar").release());

    Document::SP doc2(createDocument(content, "id:footype:testdoctype1:n=4567:bar").release());

    document::BucketIdFactory factory;
    document::BucketId bucket1(16, factory.getBucketId(doc1->getId()).getRawId());
    document::BucketId bucket2(16, factory.getBucketId(doc2->getId()).getRawId());

    // Populate bucket with the given data
    for (uint32_t i = 1; i < 10; i++) {
        filestorHandler.schedule(
                api::StorageMessage::SP(new api::PutCommand(makeDocumentBucket(bucket1), doc1, i)), 0);
        filestorHandler.schedule(
                api::StorageMessage::SP(new api::PutCommand(makeDocumentBucket(bucket2), doc2, i + 10)), 0);
    }

    {
        FileStorHandler::LockedMessage lock = filestorHandler.getNextMessage(0, stripeId);
        ASSERT_EQ(1, getPutTime(lock.second));

        lock = filestorHandler.getNextMessage(0, stripeId, lock);
        ASSERT_EQ(2, getPutTime(lock.second));

        lock = filestorHandler.getNextMessage(0, stripeId, lock);
        ASSERT_EQ(3, getPutTime(lock.second));
    }

    {
        FileStorHandler::LockedMessage lock = filestorHandler.getNextMessage(0, stripeId);
        ASSERT_EQ(11, getPutTime(lock.second));

        lock = filestorHandler.getNextMessage(0, stripeId, lock);
        ASSERT_EQ(12, getPutTime(lock.second));
    }
}

TEST_F(FileStorManagerTest, handler_timeout) {
    // Setup a filestorthread to test
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(dummyManager = new DummyStorageLink));
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);

    // Since we fake time with small numbers, we need to make sure we dont
    // compact them away, as they will seem to be from 1970

    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(),1,  1);

    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    filestorHandler.setGetNextMessageTimeout(50);
    uint32_t stripeId = filestorHandler.getNextStripeId(0);

    std::string content("Here is some content which is in all documents");
    std::ostringstream uri;

    Document::SP doc(createDocument(content, "id:footype:testdoctype1:n=1234:bar").release());

    document::BucketIdFactory factory;
    document::BucketId bucket(16, factory.getBucketId(doc->getId()).getRawId());

    // Populate bucket with the given data
    {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bucket), doc, 100);
        auto address = std::make_unique<api::StorageMessageAddress>("storage", lib::NodeType::STORAGE, 3);
        cmd->setAddress(*address);
        cmd->setPriority(0);
        cmd->setTimeout(50);
        filestorHandler.schedule(cmd, 0);
    }

    {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bucket), doc, 100);
        auto address = std::make_unique<api::StorageMessageAddress>("storage", lib::NodeType::STORAGE, 3);
        cmd->setAddress(*address);
        cmd->setPriority(200);
        cmd->setTimeout(10000);
        filestorHandler.schedule(cmd, 0);
    }

    FastOS_Thread::Sleep(51);
    for (;;) {
        auto lock = filestorHandler.getNextMessage(0, stripeId);
        if (lock.first.get()) {
            ASSERT_EQ(200, lock.second->getPriority());
            break;
        }
    }

    ASSERT_EQ(1, top.getNumReplies());
    EXPECT_EQ(api::ReturnCode::TIMEOUT,
              static_cast<api::StorageReply&>(*top.getReply(0)).getResult().getResult());
}

TEST_F(FileStorManagerTest, priority) {
    // Setup a filestorthread to test
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(
                          dummyManager = new DummyStorageLink));
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);
    // Since we fake time with small numbers, we need to make sure we dont
    // compact them away, as they will seem to be from 1970

    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(),1,  2);

    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    std::unique_ptr<DiskThread> thread(createThread(
            *config, *_node, _node->getPersistenceProvider(),
            filestorHandler, *metrics.disks[0]->threads[0], 0));
    std::unique_ptr<DiskThread> thread2(createThread(
            *config, *_node, _node->getPersistenceProvider(),
            filestorHandler, *metrics.disks[0]->threads[1], 0));

    // Creating documents to test with. Different gids, 2 locations.
    std::vector<document::Document::SP > documents;
    for (uint32_t i=0; i<50; ++i) {
        std::string content("Here is some content which is in all documents");
        std::ostringstream uri;

        uri << "id:footype:testdoctype1:n=" << (i % 3 == 0 ? 0x10001 : 0x0100001)<< ":mydoc-" << i;
        Document::SP doc(createDocument(content, uri.str()).release());
        documents.push_back(doc);
    }

    document::BucketIdFactory factory;

    // Create buckets in separate, initial pass to avoid races with puts
    for (uint32_t i=0; i<documents.size(); ++i) {
        document::BucketId bucket(16, factory.getBucketId(documents[i]->getId()).getRawId());

        spi::Context context(defaultLoadType, spi::Priority(0), spi::Trace::TraceLevel(0));

        _node->getPersistenceProvider().createBucket(makeSpiBucket(bucket), context);
    }

    // Populate bucket with the given data
    for (uint32_t i=0; i<documents.size(); ++i) {
        document::BucketId bucket(16, factory.getBucketId(documents[i]->getId()).getRawId());

        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bucket), documents[i], 100 + i);
        auto address = std::make_unique<api::StorageMessageAddress>("storage", lib::NodeType::STORAGE, 3);
        cmd->setAddress(*address);
        cmd->setPriority(i * 2);
        filestorHandler.schedule(cmd, 0);
    }

    filestorHandler.flush(true);

    // Wait until everything is done.
    int count = 0;
    while (documents.size() != top.getNumReplies() && count < 10000) {
        FastOS_Thread::Sleep(10);
        count++;
    }
    ASSERT_LT(count, 10000);

    for (uint32_t i = 0; i < documents.size(); i++) {
        std::shared_ptr<api::PutReply> reply(
                std::dynamic_pointer_cast<api::PutReply>(
                        top.getReply(i)));
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
    }

    // Verify that thread 1 gets documents over 50 pri
    EXPECT_EQ(documents.size(),
              metrics.disks[0]->threads[0]->operations.getValue()
              + metrics.disks[0]->threads[1]->operations.getValue());
    // Closing file stor handler before threads are deleted, such that
    // file stor threads getNextMessage calls returns.
    filestorHandler.close();
}

TEST_F(FileStorManagerTest, split1) {
    // Setup a filestorthread to test
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(
                dummyManager = new DummyStorageLink));
    setClusterState("storage:2 distributor:1");
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);
    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(), 1, 1);
    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    std::unique_ptr<DiskThread> thread(createThread(
            *config, *_node, _node->getPersistenceProvider(),
            filestorHandler, *metrics.disks[0]->threads[0], 0));
    // Creating documents to test with. Different gids, 2 locations.
    std::vector<document::Document::SP > documents;
    for (uint32_t i=0; i<20; ++i) {
        std::string content("Here is some content which is in all documents");
        std::ostringstream uri;

        uri << "id:footype:testdoctype1:n=" << (i % 3 == 0 ? 0x10001 : 0x0100001)
                               << ":mydoc-" << i;
        Document::SP doc(createDocument(
                content, uri.str()).release());
        documents.push_back(doc);
    }
    document::BucketIdFactory factory;
    spi::Context context(defaultLoadType, spi::Priority(0),
                         spi::Trace::TraceLevel(0));
    {
        // Populate bucket with the given data
        for (uint32_t i=0; i<documents.size(); ++i) {
            document::BucketId bucket(16, factory.getBucketId(
                                             documents[i]->getId()).getRawId());

            _node->getPersistenceProvider().createBucket(
                    makeSpiBucket(bucket), context);

            auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bucket), documents[i], 100 + i);
            auto address = std::make_unique<api::StorageMessageAddress>("storage", lib::NodeType::STORAGE, 3);
            cmd->setAddress(*address);
            cmd->setSourceIndex(0);

            filestorHandler.schedule(cmd, 0);
            filestorHandler.flush(true);
            LOG(debug, "Got %zu replies", top.getNumReplies());
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
            top.reset();

            // Delete every 5th document to have delete entries in file too
            if (i % 5 == 0) {
                auto rcmd = std::make_shared<api::RemoveCommand>(
                        makeDocumentBucket(bucket), documents[i]->getId(), 1000000 + 100 + i);
                rcmd->setAddress(*address);
                filestorHandler.schedule(rcmd, 0);
                filestorHandler.flush(true);
                ASSERT_EQ(1, top.getNumReplies());
                auto rreply = std::dynamic_pointer_cast<api::RemoveReply>(top.getReply(0));
                ASSERT_TRUE(rreply.get()) << top.getReply(0)->getType().toString();
                EXPECT_EQ(ReturnCode(ReturnCode::OK), rreply->getResult());
                top.reset();
            }
        }

        // Perform a split, check that locations are split
        {
            auto cmd = std::make_shared<api::SplitBucketCommand>(makeDocumentBucket(document::BucketId(16, 1)));
            cmd->setSourceIndex(0);
            filestorHandler.schedule(cmd, 0);
            filestorHandler.flush(true);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<api::SplitBucketReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
            top.reset();
        }

        // Test that the documents have gotten into correct parts.
        for (uint32_t i=0; i<documents.size(); ++i) {
            document::BucketId bucket(
                        17, i % 3 == 0 ? 0x10001 : 0x0100001);
            auto cmd = std::make_shared<api::GetCommand>(
                    makeDocumentBucket(bucket), documents[i]->getId(), "[all]");
            api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
            cmd->setAddress(address);
            filestorHandler.schedule(cmd, 0);
            filestorHandler.flush(true);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<api::GetReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(((i % 5) != 0), reply->wasFound());
            top.reset();
        }

        // Keep splitting location 1 until we gidsplit
        for (int i=17; i<=32; ++i) {
            auto cmd = std::make_shared<api::SplitBucketCommand>(
                        makeDocumentBucket(document::BucketId(i, 0x0100001)));
            cmd->setSourceIndex(0);
            filestorHandler.schedule(cmd, 0);
            filestorHandler.flush(true);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<api::SplitBucketReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
            top.reset();
        }

        // Test that the documents have gotten into correct parts.
        for (uint32_t i=0; i<documents.size(); ++i) {
            document::BucketId bucket;
            if (i % 3 == 0) {
                bucket = document::BucketId(17, 0x10001);
            } else {
                bucket = document::BucketId(33, factory.getBucketId(
                                    documents[i]->getId()).getRawId());
            }
            auto cmd = std::make_shared<api::GetCommand>(
                    makeDocumentBucket(bucket), documents[i]->getId(), "[all]");
            api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
            cmd->setAddress(address);
            filestorHandler.schedule(cmd, 0);
            filestorHandler.flush(true);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<api::GetReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(((i % 5) != 0), reply->wasFound());
            top.reset();
        }
    }
        // Closing file stor handler before threads are deleted, such that
        // file stor threads getNextMessage calls returns.
    filestorHandler.close();
}

TEST_F(FileStorManagerTest, split_single_group) {
    // Setup a filestorthread to test
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(
                dummyManager = new DummyStorageLink));
    setClusterState("storage:2 distributor:1");
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);
    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(),1,  1);
    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    spi::Context context(defaultLoadType, spi::Priority(0), spi::Trace::TraceLevel(0));
    for (uint32_t j=0; j<1; ++j) {
        // Test this twice, once where all the data ends up in file with
        // splitbit set, and once where all the data ends up in file with
        // splitbit unset
        bool state = (j == 0);

        std::unique_ptr<DiskThread> thread(createThread(
                *config, *_node, _node->getPersistenceProvider(),
                filestorHandler, *metrics.disks[0]->threads[0], 0));
        // Creating documents to test with. Different gids, 2 locations.
        std::vector<document::Document::SP> documents;
        for (uint32_t i=0; i<20; ++i) {
            std::string content("Here is some content for all documents");
            std::ostringstream uri;

            uri << "id:footype:testdoctype1:n=" << (state ? 0x10001 : 0x0100001)
                                   << ":mydoc-" << i;
            documents.emplace_back(createDocument(content, uri.str()));
        }
        document::BucketIdFactory factory;

        // Populate bucket with the given data
        for (uint32_t i=0; i<documents.size(); ++i) {
            document::BucketId bucket(16, factory.getBucketId(
                                documents[i]->getId()).getRawId());

            _node->getPersistenceProvider().createBucket(makeSpiBucket(bucket), context);

            auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bucket), documents[i], 100 + i);
            api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
            cmd->setAddress(address);
            filestorHandler.schedule(cmd, 0);
            filestorHandler.flush(true);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
            top.reset();
        }
        // Perform a split, check that locations are split
        {
            auto cmd = std::make_shared<api::SplitBucketCommand>(makeDocumentBucket(document::BucketId(16, 1)));
            cmd->setSourceIndex(0);
            filestorHandler.schedule(cmd, 0);
            filestorHandler.flush(true);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<api::SplitBucketReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
            top.reset();
        }

        // Test that the documents are all still there
        for (uint32_t i=0; i<documents.size(); ++i) {
            document::BucketId bucket(17, state ? 0x10001 : 0x00001);
            auto cmd = std::make_shared<api::GetCommand>
                    (makeDocumentBucket(bucket), documents[i]->getId(), "[all]");
            api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
            cmd->setAddress(address);
            filestorHandler.schedule(cmd, 0);
            filestorHandler.flush(true);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<api::GetReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
            top.reset();
        }
            // Closing file stor handler before threads are deleted, such that
            // file stor threads getNextMessage calls returns.
        filestorHandler.close();
    }
}

void
FileStorManagerTest::putDoc(DummyStorageLink& top,
                            FileStorHandler& filestorHandler,
                            const document::BucketId& target,
                            uint32_t docNum)
{
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
    spi::Context context(defaultLoadType, spi::Priority(0),
                         spi::Trace::TraceLevel(0));
    document::BucketIdFactory factory;
    document::DocumentId docId(vespalib::make_string("id:ns:testdoctype1:n=%" PRIu64 ":%d", target.getId(), docNum));
    document::BucketId bucket(16, factory.getBucketId(docId).getRawId());
    //std::cerr << "doc bucket is " << bucket << " vs source " << source << "\n";
    _node->getPersistenceProvider().createBucket(
            makeSpiBucket(target), context);
    Document::SP doc(new Document(*_testdoctype1, docId));
    std::shared_ptr<api::PutCommand> cmd(
            new api::PutCommand(makeDocumentBucket(target), doc, docNum+1));
    cmd->setAddress(address);
    cmd->setPriority(120);
    filestorHandler.schedule(cmd, 0);
    filestorHandler.flush(true);
    ASSERT_EQ(1, top.getNumReplies());
    std::shared_ptr<api::PutReply> reply(
            std::dynamic_pointer_cast<api::PutReply>(
                    top.getReply(0)));
    ASSERT_TRUE(reply.get());
    ASSERT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
    top.reset();
}

TEST_F(FileStorManagerTest, split_empty_target_with_remapped_ops) {
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(
                dummyManager = new DummyStorageLink));
    setClusterState("storage:2 distributor:1");
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);
    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(), 1, 1);
    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    std::unique_ptr<DiskThread> thread(createThread(
            *config, *_node, _node->getPersistenceProvider(),
            filestorHandler, *metrics.disks[0]->threads[0], 0));

    document::BucketId source(16, 0x10001);

    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);

    for (uint32_t i=0; i<10; ++i) {
        ASSERT_NO_FATAL_FAILURE(putDoc(top, filestorHandler, source, i));
    }

    // Send split followed by a put that is bound for a target bucket that
    // will end up empty in the split itself. The split should notice this
    // and create the bucket explicitly afterwards in order to compensate for
    // the persistence provider deleting it internally.
    // Make sure we block the operation queue until we've scheduled all
    // the operations.
    auto resumeGuard = std::make_unique<ResumeGuard>(filestorHandler.pause());

    auto splitCmd = std::make_shared<api::SplitBucketCommand>(makeDocumentBucket(source));
    splitCmd->setPriority(120);
    splitCmd->setSourceIndex(0);

    document::DocumentId docId(
            vespalib::make_string("id:ns:testdoctype1:n=%d:1234", 0x100001));
    auto doc = std::make_shared<Document>(*_testdoctype1, docId);
    auto putCmd = std::make_shared<api::PutCommand>(makeDocumentBucket(source), doc, 1001);
    putCmd->setAddress(address);
    putCmd->setPriority(120);

    filestorHandler.schedule(splitCmd, 0);
    filestorHandler.schedule(putCmd, 0);
    resumeGuard.reset(); // Unpause
    filestorHandler.flush(true);

    top.waitForMessages(2, _waitTime);

    ASSERT_EQ(2, top.getNumReplies());
    {
        auto reply = std::dynamic_pointer_cast<api::SplitBucketReply>(top.getReply(0));
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
    }
    {
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(1));
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
    }

    top.reset();
}

TEST_F(FileStorManagerTest, notify_on_split_source_ownership_changed) {
    // Setup a filestorthread to test
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(dummyManager = new DummyStorageLink));
    setClusterState("storage:2 distributor:2");
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);
    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(), 1, 1);
    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    std::unique_ptr<DiskThread> thread(createThread(
            *config, *_node, _node->getPersistenceProvider(),
            filestorHandler, *metrics.disks[0]->threads[0], 0));

    document::BucketId source(getFirstBucketNotOwnedByDistributor(0));
    createBucket(source, 0);
    for (uint32_t i=0; i<10; ++i) {
        ASSERT_NO_FATAL_FAILURE(putDoc(top, filestorHandler, source, i));
    }

    auto splitCmd = std::make_shared<api::SplitBucketCommand>(makeDocumentBucket(source));
    splitCmd->setPriority(120);
    splitCmd->setSourceIndex(0); // Source not owned by this distributor.

    filestorHandler.schedule(splitCmd, 0);
    filestorHandler.flush(true);
    top.waitForMessages(4, _waitTime); // 3 notify cmds + split reply

    ASSERT_EQ(4, top.getNumReplies());
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(api::MessageType::NOTIFYBUCKETCHANGE, top.getReply(i)->getType());
    }

    auto reply = std::dynamic_pointer_cast<api::SplitBucketReply>(top.getReply(3));
    ASSERT_TRUE(reply.get());
    EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
}

TEST_F(FileStorManagerTest, join) {
    // Setup a filestorthread to test
    DummyStorageLink top;
    DummyStorageLink *dummyManager;
    top.push_back(std::unique_ptr<StorageLink>(
                dummyManager = new DummyStorageLink));
    top.open();
    ForwardingMessageSender messageSender(*dummyManager);

    documentapi::LoadTypeSet loadTypes("raw:");
    FileStorMetrics metrics(loadTypes.getMetricLoadTypes());
    metrics.initDiskMetrics(_node->getPartitions().size(), loadTypes.getMetricLoadTypes(), 1, 1);
    FileStorHandler filestorHandler(messageSender, metrics, _node->getPartitions(), _node->getComponentRegister());
    std::unique_ptr<DiskThread> thread(createThread(
            *config, *_node, _node->getPersistenceProvider(),
            filestorHandler, *metrics.disks[0]->threads[0], 0));
    // Creating documents to test with. Different gids, 2 locations.
    std::vector<document::Document::SP > documents;
    for (uint32_t i=0; i<20; ++i) {
        std::string content("Here is some content which is in all documents");
        std::ostringstream uri;
        uri << "id:footype:testdoctype1:n=" << (i % 3 == 0 ? 0x10001 : 0x0100001) << ":mydoc-" << i;
        documents.emplace_back(createDocument(content, uri.str()));
    }
    document::BucketIdFactory factory;

    createBucket(document::BucketId(17, 0x00001), 0);
    createBucket(document::BucketId(17, 0x10001), 0);

    {
        // Populate bucket with the given data
        for (uint32_t i=0; i<documents.size(); ++i) {
            document::BucketId bucket(17, factory.getBucketId(documents[i]->getId()).getRawId());
            auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bucket), documents[i], 100 + i);
            auto address = std::make_unique<api::StorageMessageAddress>("storage", lib::NodeType::STORAGE, 3);
            cmd->setAddress(*address);
            filestorHandler.schedule(cmd, 0);
            filestorHandler.flush(true);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
            top.reset();
            // Delete every 5th document to have delete entries in file too
            if ((i % 5) == 0) {
                auto rcmd = std::make_shared<api::RemoveCommand>(
                        makeDocumentBucket(bucket), documents[i]->getId(), 1000000 + 100 + i);
                rcmd->setAddress(*address);
                filestorHandler.schedule(rcmd, 0);
                filestorHandler.flush(true);
                ASSERT_EQ(1, top.getNumReplies());
                auto rreply = std::dynamic_pointer_cast<api::RemoveReply>(top.getReply(0));
                ASSERT_TRUE(rreply.get()) << top.getReply(0)->getType().toString();
                EXPECT_EQ(ReturnCode(ReturnCode::OK), rreply->getResult());
                top.reset();
            }
        }
        LOG(debug, "Starting the actual join after populating data");
        // Perform a join, check that other files are gone
        {
            auto cmd = std::make_shared<api::JoinBucketsCommand>(makeDocumentBucket(document::BucketId(16, 1)));
            cmd->getSourceBuckets().emplace_back(document::BucketId(17, 0x00001));
            cmd->getSourceBuckets().emplace_back(document::BucketId(17, 0x10001));
            filestorHandler.schedule(cmd, 0);
            filestorHandler.flush(true);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<api::JoinBucketsReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
            top.reset();
        }
            // Test that the documents have gotten into the file.
        for (uint32_t i=0; i<documents.size(); ++i) {
            document::BucketId bucket(16, 1);
            auto cmd = std::make_shared<api::GetCommand>(
                    makeDocumentBucket(bucket), documents[i]->getId(), "[all]");
            api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
            cmd->setAddress(address);
            filestorHandler.schedule(cmd, 0);
            filestorHandler.flush(true);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<api::GetReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(((i % 5) != 0), reply->wasFound());
            top.reset();
        }
    }
    // Closing file stor handler before threads are deleted, such that
    // file stor threads getNextMessage calls returns.
    filestorHandler.close();
}

namespace {

spi::IteratorId
createIterator(DummyStorageLink& link,
               const document::BucketId& bucketId,
               const std::string& docSel,
               framework::MicroSecTime fromTime = framework::MicroSecTime(0),
               framework::MicroSecTime toTime = framework::MicroSecTime::max(),
               bool headerOnly = false)
{
    spi::Bucket bucket(makeSpiBucket(bucketId));

    spi::Selection selection =
        spi::Selection(spi::DocumentSelection(docSel));
    selection.setFromTimestamp(spi::Timestamp(fromTime.getTime()));
    selection.setToTimestamp(spi::Timestamp(toTime.getTime()));
    auto createIterCmd = std::make_shared<CreateIteratorCommand>(
            makeDocumentBucket(bucket), selection,
            headerOnly ? "[header]" : "[all]",
            spi::NEWEST_DOCUMENT_ONLY);
    link.sendDown(createIterCmd);
    link.waitForMessages(1, FileStorManagerTest::LONG_WAITTIME);
    assert(link.getNumReplies() == 1);
    auto reply = std::dynamic_pointer_cast<CreateIteratorReply>(link.getReply(0));
    assert(reply.get());
    link.reset();
    assert(reply->getResult().success());
    return reply->getIteratorId();
}

}

TEST_F(FileStorManagerTest, visiting) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager = new FileStorManager(
            smallConfig->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
        // Adding documents to two buckets which we are going to visit
        // We want one bucket in one slotfile, and one bucket with a file split
    uint32_t docCount = 50;
    std::vector<document::BucketId> ids = {
        document::BucketId(16, 1),
        document::BucketId(16, 2)
    };

    createBucket(ids[0], 0);
    createBucket(ids[1], 0);

    lib::RandomGen randomizer(523);
    for (uint32_t i=0; i<docCount; ++i) {
        std::string content("Here is some content which is in all documents");
        std::ostringstream uri;

        uri << "id:crawler:testdoctype1:n=" << (i < 3 ? 1 : 2) << ":"
            << randomizer.nextUint32() << ".html";
        Document::SP doc(createDocument(content, uri.str()));
        const document::DocumentType& type(doc->getType());
        if (i < 30) {
            doc->setValue(type.getField("hstringval"),
                          document::StringFieldValue("John Doe"));
        } else {
            doc->setValue(type.getField("hstringval"),
                          document::StringFieldValue("Jane Doe"));
        }
        auto cmd = std::make_shared<api::PutCommand>(
                makeDocumentBucket(ids[(i < 3) ? 0 : 1]), doc, i+1);
        top.sendDown(cmd);
    }
    top.waitForMessages(docCount, _waitTime);
    ASSERT_EQ(docCount, top.getNumReplies());
    // Check nodestate with splitting
    {
        api::BucketInfo info;
        for (uint32_t i=3; i<docCount; ++i) {
            auto reply = std::dynamic_pointer_cast<api::BucketInfoReply>(top.getReply(i));
            ASSERT_TRUE(reply.get());
            ASSERT_TRUE(reply->getResult().success()) << reply->getResult().toString();

            info = reply->getBucketInfo();
        }
        EXPECT_EQ(docCount - 3, info.getDocumentCount());
    }
    top.reset();
    // Visit bucket with no split, using no selection
    {
        spi::IteratorId iterId(createIterator(top, ids[0], "true"));
        auto cmd = std::make_shared<GetIterCommand>(makeDocumentBucket(ids[0]), iterId, 16*1024);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<GetIterReply>(top.getReply(0));
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
        EXPECT_EQ(ids[0], reply->getBucketId());
        EXPECT_EQ(3, reply->getEntries().size());
        top.reset();
    }
    // Visit bucket with split, using selection
    {
        uint32_t totalDocs = 0;
        spi::IteratorId iterId(createIterator(top, ids[1], "testdoctype1.hstringval = \"John Doe\""));
        while (true) {
            auto cmd = std::make_shared<GetIterCommand>(makeDocumentBucket(ids[1]), iterId, 16*1024);
            top.sendDown(cmd);
            top.waitForMessages(1, _waitTime);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<GetIterReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
            EXPECT_EQ(ids[1], reply->getBucketId());
            totalDocs += reply->getEntries().size();
            top.reset();
            if (reply->isCompleted()) {
                break;
            }
        }
        EXPECT_EQ(27u, totalDocs);
    }
    // Visit bucket with min and max timestamps set, headers only
    {
        document::BucketId bucket(16, 2);
        spi::IteratorId iterId(
                createIterator(top,
                               ids[1],
                               "",
                               framework::MicroSecTime(30),
                               framework::MicroSecTime(40),
                               true));
        uint32_t totalDocs = 0;
        while (true) {
            auto cmd = std::make_shared<GetIterCommand>(makeDocumentBucket(ids[1]), iterId, 16*1024);
            top.sendDown(cmd);
            top.waitForMessages(1, _waitTime);
            ASSERT_EQ(1, top.getNumReplies());
            auto reply = std::dynamic_pointer_cast<GetIterReply>(top.getReply(0));
            ASSERT_TRUE(reply.get());
            EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
            EXPECT_EQ(bucket, reply->getBucketId());
            totalDocs += reply->getEntries().size();
            top.reset();
            if (reply->isCompleted()) {
                break;
            }
        }
        EXPECT_EQ(11u, totalDocs);
    }

}

TEST_F(FileStorManagerTest, remove_location) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager =
            new FileStorManager(config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
    document::BucketId bid(8, 0);

    createBucket(bid, 0);

    // Adding some documents to be removed later
    for (uint32_t i=0; i<=10; ++i) {
        std::ostringstream docid;
        docid << "id:ns:testdoctype1:n=" << (i << 8) << ":foo";
        Document::SP doc(createDocument("some content", docid.str()));
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 1000 + i);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
        EXPECT_EQ(i + 1u, reply->getBucketInfo().getDocumentCount());
    }
    // Issuing remove location command
    {
        auto cmd = std::make_shared<api::RemoveLocationCommand>("id.user % 512 == 0", makeDocumentBucket(bid));
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::RemoveLocationReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
        EXPECT_EQ(5u, reply->getBucketInfo().getDocumentCount());
    }
}

TEST_F(FileStorManagerTest, delete_bucket) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager = new FileStorManager(
                    config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 2);
    // Creating a document to test with
    document::DocumentId docId("id:crawler:testdoctype1:n=4000:http://www.ntnu.no/");
    auto doc = std::make_shared<Document>(*_testdoctype1, docId);
    document::BucketId bid(16, 4000);

    createBucket(bid, 0);

    api::BucketInfo bucketInfo;
    // Putting it
    {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 105);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());

        EXPECT_EQ(1, reply->getBucketInfo().getDocumentCount());
        bucketInfo = reply->getBucketInfo();
        top.reset();
    }

    // Delete bucket
    {
        auto cmd = std::make_shared<api::DeleteBucketCommand>(makeDocumentBucket(bid));
        cmd->setAddress(address);
        cmd->setBucketInfo(bucketInfo);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::DeleteBucketReply>(top.getReply(0));
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
    }
}

TEST_F(FileStorManagerTest, delete_bucket_rejects_outdated_bucket_info) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager = new FileStorManager(
                    config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 2);
    // Creating a document to test with
    document::DocumentId docId("id:crawler:testdoctype1:n=4000:http://www.ntnu.no/");
    Document::SP doc(new Document(*_testdoctype1, docId));
    document::BucketId bid(16, 4000);

    createBucket(bid, 0);

    api::BucketInfo bucketInfo;

    // Putting it
    {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 105);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());

        EXPECT_EQ(1, reply->getBucketInfo().getDocumentCount());
        bucketInfo = reply->getBucketInfo();
        top.reset();
    }

    // Attempt to delete bucket, but with non-matching bucketinfo
    {
        auto cmd = std::make_shared<api::DeleteBucketCommand>(makeDocumentBucket(bid));
        cmd->setBucketInfo(api::BucketInfo(0xf000baaa, 1, 123, 1, 456));
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::DeleteBucketReply>(top.getReply(0));
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode::REJECTED, reply->getResult().getResult());
        EXPECT_EQ(bucketInfo, reply->getBucketInfo());
    }
}

/**
 * Test that receiving a DeleteBucketCommand with invalid
 * BucketInfo deletes the bucket and does not fail the operation.
 */
TEST_F(FileStorManagerTest, delete_bucket_with_invalid_bucket_info){
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager = new FileStorManager(
                    config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 2);
    // Creating a document to test with
    document::DocumentId docId("id:crawler:testdoctype1:n=4000:http://www.ntnu.no/");
    auto doc = std::make_shared<Document>(*_testdoctype1, docId);
    document::BucketId bid(16, 4000);

    createBucket(bid, 0);

    // Putting it
    {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 105);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
        EXPECT_EQ(1, reply->getBucketInfo().getDocumentCount());
        top.reset();
    }

    // Attempt to delete bucket with invalid bucketinfo
    {
        auto cmd = std::make_shared<api::DeleteBucketCommand>(makeDocumentBucket(bid));
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::DeleteBucketReply>(top.getReply(0));
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode::OK, reply->getResult().getResult());
        EXPECT_EQ(api::BucketInfo(), reply->getBucketInfo());
    }
}

TEST_F(FileStorManagerTest, no_timestamps) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager =
            new FileStorManager(config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
    api::StorageMessageAddress address(
            "storage", lib::NodeType::STORAGE, 3);
    // Creating a document to test with
    Document::SP doc(createDocument(
                "some content", "doc:crawler:http://www.ntnu.no/").release());
    document::BucketId bid(16, 4000);

    createBucket(bid, 0);

    // Putting it
    {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 0);
        cmd->setAddress(address);
        EXPECT_EQ(api::Timestamp(0), cmd->getTimestamp());
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode::REJECTED, reply->getResult().getResult());
    }
    // Removing it
    {
        auto cmd = std::make_shared<api::RemoveCommand>(makeDocumentBucket(bid), doc->getId(), 0);
        cmd->setAddress(address);
        EXPECT_EQ(api::Timestamp(0), cmd->getTimestamp());
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::RemoveReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode::REJECTED, reply->getResult().getResult());
    }
}

TEST_F(FileStorManagerTest, equal_timestamps) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager =
            new FileStorManager(config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
    // Creating a document to test with
    document::BucketId bid(16, 4000);

    createBucket(bid, 0);

    // Putting it
    {
        Document::SP doc(createDocument(
                "some content", "id:crawler:testdoctype1:n=4000:http://www.ntnu.no/"));
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 100);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode::OK, reply->getResult().getResult());
    }

    // Putting it on same timestamp again
    // (ok as doc is the same. Since merge can move doc to other copy we
    // have to accept this)
    {
        Document::SP doc(createDocument(
                "some content", "id:crawler:testdoctype1:n=4000:http://www.ntnu.no/"));
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 100);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode::OK, reply->getResult().getResult());
    }

    // Putting the doc with other id. Now we should fail
    {
        Document::SP doc(createDocument(
                "some content", "id:crawler:testdoctype1:n=4000:http://www.ntnu.nu/"));
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), doc, 100);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode::TIMESTAMP_EXIST, reply->getResult().getResult());
    }
}

TEST_F(FileStorManagerTest, get_iter) {
    // Setting up manager
    DummyStorageLink top;
    FileStorManager *manager;
    top.push_back(unique_ptr<StorageLink>(manager =
            new FileStorManager(config->getConfigId(), _node->getPartitions(), _node->getPersistenceProvider(), _node->getComponentRegister())));
    top.open();
    api::StorageMessageAddress address(
            "storage", lib::NodeType::STORAGE, 3);
    document::BucketId bid(16, 4000);

    createBucket(bid, 0);

    std::vector<Document::SP > docs;
    // Creating some documents to test with
    for (uint32_t i=0; i<10; ++i) {
        std::ostringstream id;
        id << "id:crawler:testdoctype1:n=4000:http://www.ntnu.no/" << i;
        docs.emplace_back(
                Document::SP(
                    _node->getTestDocMan().createRandomDocumentAtLocation(
                        4000, i, 400, 400)));
    }
    api::BucketInfo bucketInfo;
    // Putting all docs to have something to visit
    for (uint32_t i=0; i<docs.size(); ++i) {
        auto cmd = std::make_shared<api::PutCommand>(makeDocumentBucket(bid), docs[i], 100 + i);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::PutReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
        bucketInfo = reply->getBucketInfo();
    }
    // Sending a getiter request that will only visit some of the docs
    spi::IteratorId iterId(createIterator(top, bid, ""));
    {
        auto cmd = std::make_shared<GetIterCommand>(makeDocumentBucket(bid), iterId, 2048);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<GetIterReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
        EXPECT_GT(reply->getEntries().size(), 0);
        EXPECT_LT(reply->getEntries().size(), docs.size());
    }
    // Normal case of get iter is testing through visitor tests.
    // Testing specific situation where file is deleted while visiting here
    {
        auto cmd = std::make_shared<api::DeleteBucketCommand>(makeDocumentBucket(bid));
        cmd->setBucketInfo(bucketInfo);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::DeleteBucketReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
    }
    {
        auto cmd = std::make_shared<GetIterCommand>(makeDocumentBucket(bid), iterId, 2048);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<GetIterReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode::BUCKET_NOT_FOUND, reply->getResult().getResult());
        EXPECT_TRUE(reply->getEntries().empty());
    }
}

TEST_F(FileStorManagerTest, set_bucket_active_state) {
    DummyStorageLink top;
    FileStorManager* manager(
            new FileStorManager(config->getConfigId(),
                                _node->getPartitions(),
                                _node->getPersistenceProvider(),
                                _node->getComponentRegister()));
    top.push_back(unique_ptr<StorageLink>(manager));
    setClusterState("storage:4 distributor:1");
    top.open();
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);

    document::BucketId bid(16, 4000);

    const uint16_t disk = 0;
    createBucket(bid, disk);
    auto& provider = dynamic_cast<spi::dummy::DummyPersistence&>(_node->getPersistenceProvider());
    EXPECT_FALSE(provider.isActive(makeSpiBucket(bid, spi::PartitionId(disk))));

    {
        auto cmd = std::make_shared<api::SetBucketStateCommand>(
                makeDocumentBucket(bid), api::SetBucketStateCommand::ACTIVE);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::SetBucketStateReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
    }

    EXPECT_TRUE(provider.isActive(makeSpiBucket(bid, spi::PartitionId(disk))));
    {
        StorBucketDatabase::WrappedEntry entry(
                _node->getStorageBucketDatabase().get(
                        bid, "foo"));
        EXPECT_TRUE(entry->info.isActive());
    }
    // Trigger bucket info to be read back into the database
    {
        auto cmd = std::make_shared<ReadBucketInfo>(makeDocumentBucket(bid));
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<ReadBucketInfoReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
    }
    // Should not have lost active flag
    {
        StorBucketDatabase::WrappedEntry entry(
                _node->getStorageBucketDatabase().get(
                        bid, "foo"));
        EXPECT_TRUE(entry->info.isActive());
    }

    {
        auto cmd = std::make_shared<api::SetBucketStateCommand>(
                makeDocumentBucket(bid), api::SetBucketStateCommand::INACTIVE);
        cmd->setAddress(address);
        top.sendDown(cmd);
        top.waitForMessages(1, _waitTime);
        ASSERT_EQ(1, top.getNumReplies());
        auto reply = std::dynamic_pointer_cast<api::SetBucketStateReply>(top.getReply(0));
        top.reset();
        ASSERT_TRUE(reply.get());
        EXPECT_EQ(ReturnCode(ReturnCode::OK), reply->getResult());
    }

    EXPECT_FALSE(provider.isActive(makeSpiBucket(bid, spi::PartitionId(disk))));
    {
        StorBucketDatabase::WrappedEntry entry(
                _node->getStorageBucketDatabase().get(
                        bid, "foo"));
        EXPECT_FALSE(entry->info.isActive());
    }
}

TEST_F(FileStorManagerTest, notify_owner_distributor_on_outdated_set_bucket_state) {
    DummyStorageLink top;
    FileStorManager* manager(
            new FileStorManager(config->getConfigId(),
                                _node->getPartitions(),
                                _node->getPersistenceProvider(),
                                _node->getComponentRegister()));
    top.push_back(unique_ptr<StorageLink>(manager));

    setClusterState("storage:2 distributor:2");
    top.open();
    
    document::BucketId bid(getFirstBucketNotOwnedByDistributor(0));
    ASSERT_NE(bid.getRawId(), 0);
    createBucket(bid, 0);

    auto cmd = std::make_shared<api::SetBucketStateCommand>(
            makeDocumentBucket(bid), api::SetBucketStateCommand::ACTIVE);
    cmd->setAddress(api::StorageMessageAddress("cluster", lib::NodeType::STORAGE, 1));
    cmd->setSourceIndex(0);

    top.sendDown(cmd);
    top.waitForMessages(2, _waitTime);

    ASSERT_EQ(2, top.getNumReplies());
    // Not necessarily deterministic order.
    int idxOffset = 0;
    if (top.getReply(0)->getType() != api::MessageType::NOTIFYBUCKETCHANGE) {
        ++idxOffset;
    }
    auto notifyCmd  = std::dynamic_pointer_cast<api::NotifyBucketChangeCommand>(top.getReply(idxOffset));
    auto stateReply = std::dynamic_pointer_cast<api::SetBucketStateReply>(top.getReply(1 - idxOffset));

    ASSERT_TRUE(stateReply.get());
    EXPECT_EQ(ReturnCode(ReturnCode::OK), stateReply->getResult());

    ASSERT_TRUE(notifyCmd.get());
    EXPECT_EQ(1, notifyCmd->getAddress()->getIndex());
    // Not necessary for this to be set since distributor does not insert this
    // info into its db, but useful for debugging purposes.
    EXPECT_TRUE(notifyCmd->getBucketInfo().isActive());
}

TEST_F(FileStorManagerTest, GetBucketDiff_implicitly_creates_bucket) {
    DummyStorageLink top;
    FileStorManager* manager(
            new FileStorManager(config->getConfigId(),
                                _node->getPartitions(),
                                _node->getPersistenceProvider(),
                                _node->getComponentRegister()));
    top.push_back(unique_ptr<StorageLink>(manager));
    setClusterState("storage:2 distributor:1");
    top.open();

    document::BucketId bid(16, 4000);

    std::vector<api::MergeBucketCommand::Node> nodes = {1, 0};

    auto cmd = std::make_shared<api::GetBucketDiffCommand>(makeDocumentBucket(bid), nodes, Timestamp(1000));
    cmd->setAddress(api::StorageMessageAddress("cluster", lib::NodeType::STORAGE, 1));
    cmd->setSourceIndex(0);
    top.sendDown(cmd);

    api::GetBucketDiffReply* reply;
    ASSERT_SINGLE_REPLY(api::GetBucketDiffReply, reply, top, _waitTime);
    EXPECT_EQ(api::ReturnCode(api::ReturnCode::OK), reply->getResult());
    {
        StorBucketDatabase::WrappedEntry entry(
                _node->getStorageBucketDatabase().get(
                        bid, "foo"));
        ASSERT_TRUE(entry.exist());
        EXPECT_TRUE(entry->info.isReady());
    }
}

TEST_F(FileStorManagerTest, merge_bucket_implicitly_creates_bucket) {
    DummyStorageLink top;
    FileStorManager* manager(
            new FileStorManager(config->getConfigId(),
                                _node->getPartitions(),
                                _node->getPersistenceProvider(),
                                _node->getComponentRegister()));
    top.push_back(unique_ptr<StorageLink>(manager));
    setClusterState("storage:3 distributor:1");
    top.open();

    document::BucketId bid(16, 4000);

    std::vector<api::MergeBucketCommand::Node> nodes = {1, 2};

    auto cmd = std::make_shared<api::MergeBucketCommand>(makeDocumentBucket(bid), nodes, Timestamp(1000));
    cmd->setAddress(api::StorageMessageAddress("cluster", lib::NodeType::STORAGE, 1));
    cmd->setSourceIndex(0);
    top.sendDown(cmd);

    api::GetBucketDiffCommand* diffCmd;
    ASSERT_SINGLE_REPLY(api::GetBucketDiffCommand, diffCmd, top, _waitTime);
    {
        StorBucketDatabase::WrappedEntry entry(
                _node->getStorageBucketDatabase().get(
                        bid, "foo"));
        ASSERT_TRUE(entry.exist());
        EXPECT_TRUE(entry->info.isReady());
    }
}

TEST_F(FileStorManagerTest, newly_created_bucket_is_ready) {
    DummyStorageLink top;
    FileStorManager* manager(
            new FileStorManager(config->getConfigId(),
                                _node->getPartitions(),
                                _node->getPersistenceProvider(),
                                _node->getComponentRegister()));
    top.push_back(unique_ptr<StorageLink>(manager));
    setClusterState("storage:2 distributor:1");
    top.open();

    document::BucketId bid(16, 4000);

    auto cmd = std::make_shared<api::CreateBucketCommand>(makeDocumentBucket(bid));
    cmd->setAddress(api::StorageMessageAddress("cluster", lib::NodeType::STORAGE, 1));
    cmd->setSourceIndex(0);
    top.sendDown(cmd);

    api::CreateBucketReply* reply;
    ASSERT_SINGLE_REPLY(api::CreateBucketReply, reply, top, _waitTime);
    EXPECT_EQ(api::ReturnCode(api::ReturnCode::OK), reply->getResult());
    {
        StorBucketDatabase::WrappedEntry entry(
                _node->getStorageBucketDatabase().get(
                        bid, "foo"));
        ASSERT_TRUE(entry.exist());
        EXPECT_TRUE(entry->info.isReady());
        EXPECT_FALSE(entry->info.isActive());
    }
}

TEST_F(FileStorManagerTest, create_bucket_sets_active_flag_in_database_and_reply) {
    TestFileStorComponents c(*this);
    setClusterState("storage:2 distributor:1");

    document::BucketId bid(16, 4000);
    std::shared_ptr<api::CreateBucketCommand> cmd(
            new api::CreateBucketCommand(makeDocumentBucket(bid)));
    cmd->setAddress(api::StorageMessageAddress(
                            "cluster", lib::NodeType::STORAGE, 1));
    cmd->setSourceIndex(0);
    cmd->setActive(true);
    c.top.sendDown(cmd);

    api::CreateBucketReply* reply;
    ASSERT_SINGLE_REPLY(api::CreateBucketReply, reply, c.top, _waitTime);
    EXPECT_EQ(api::ReturnCode(api::ReturnCode::OK), reply->getResult());
    {
        StorBucketDatabase::WrappedEntry entry(
                _node->getStorageBucketDatabase().get(
                        bid, "foo"));
        ASSERT_TRUE(entry.exist());
        EXPECT_TRUE(entry->info.isReady());
        EXPECT_TRUE(entry->info.isActive());
    }
}

template <typename Metric>
void FileStorManagerTest::assert_request_size_set(TestFileStorComponents& c, std::shared_ptr<api::StorageMessage> cmd, const Metric& metric) {
    api::StorageMessageAddress address("storage", lib::NodeType::STORAGE, 3);
    cmd->setApproxByteSize(54321);
    cmd->setAddress(address);
    c.top.sendDown(cmd);
    c.top.waitForMessages(1, _waitTime);
    EXPECT_EQ(static_cast<int64_t>(cmd->getApproxByteSize()), metric.request_size.getLast());
}

TEST_F(FileStorManagerTest, put_command_size_is_added_to_metric) {
    TestFileStorComponents c(*this);
    document::BucketId bucket(16, 4000);
    createBucket(bucket, 0);
    auto cmd = std::make_shared<api::PutCommand>(
            makeDocumentBucket(bucket), _node->getTestDocMan().createRandomDocument(), api::Timestamp(12345));

    assert_request_size_set(c, std::move(cmd), thread_metrics_of(*c.manager)->put[defaultLoadType]);
}

TEST_F(FileStorManagerTest, update_command_size_is_added_to_metric) {
    TestFileStorComponents c(*this);
    document::BucketId bucket(16, 4000);
    createBucket(bucket, 0);
    auto update = std::make_shared<document::DocumentUpdate>(
            _node->getTestDocMan().getTypeRepo(),
            _node->getTestDocMan().createRandomDocument()->getType(),
            document::DocumentId("id:foo:testdoctype1::bar"));
    auto cmd = std::make_shared<api::UpdateCommand>(
            makeDocumentBucket(bucket), std::move(update), api::Timestamp(123456));

    assert_request_size_set(c, std::move(cmd), thread_metrics_of(*c.manager)->update[defaultLoadType]);
}

TEST_F(FileStorManagerTest, remove_command_size_is_added_to_metric) {
    TestFileStorComponents c(*this);
    document::BucketId bucket(16, 4000);
    createBucket(bucket, 0);
    auto cmd = std::make_shared<api::RemoveCommand>(
            makeDocumentBucket(bucket), document::DocumentId("id:foo:testdoctype1::bar"), api::Timestamp(123456));

    assert_request_size_set(c, std::move(cmd), thread_metrics_of(*c.manager)->remove[defaultLoadType]);
}

TEST_F(FileStorManagerTest, get_command_size_is_added_to_metric) {
    TestFileStorComponents c(*this);
    document::BucketId bucket(16, 4000);
    createBucket(bucket, 0);
    auto cmd = std::make_shared<api::GetCommand>(
            makeDocumentBucket(bucket), document::DocumentId("id:foo:testdoctype1::bar"), "[all]");

    assert_request_size_set(c, std::move(cmd), thread_metrics_of(*c.manager)->get[defaultLoadType]);
}

} // storage

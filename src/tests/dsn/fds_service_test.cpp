#include <gtest/gtest.h>

#include <dsn/utility/filesystem.h>
#include <dsn/dist/block_service.h>
#include <memory>
#include <fstream>

#include "dist/block_service/fds/fds_service.h"

using namespace dsn;
using namespace dsn::dist::block_service;

static void pipe_execute(const char *command, std::stringstream &output)
{
    std::array<char, 256> buffer;

    std::shared_ptr<FILE> command_pipe(popen(command, "r"), pclose);
    while (!feof(command_pipe.get())) {
        if (fgets(buffer.data(), 256, command_pipe.get()) != NULL)
            output << buffer.data();
    }
}

static void file_eq_compare(const std::string &fname1, const std::string &fname2)
{
    static const int length = 4096;
    std::shared_ptr<char> buffer(dsn::utils::make_shared_array<char>(length * 2));
    char *buf1 = buffer.get(), *buf2 = buffer.get() + length;

    std::ifstream ifile1(fname1.c_str(), std::ios::in | std::ios::binary);
    std::ifstream ifile2(fname2.c_str(), std::ios::in | std::ios::binary);

    auto file_length = [](std::ifstream &is) {
        is.seekg(0, is.end);
        int result = is.tellg();
        is.seekg(0, is.beg);
        return result;
    };

    int l = file_length(ifile1);
    ASSERT_EQ(l, file_length(ifile2));

    for (int i = 0; i < l; i += length) {
        int up_to_bytes = length < (l - i) ? length : (l - i);
        ifile1.read(buf1, up_to_bytes);
        ifile2.read(buf2, up_to_bytes);
        ASSERT_TRUE(memcmp(buf1, buf2, up_to_bytes) == 0);
    }
}

class FDSClientTest : public testing::Test
{
protected:
    virtual void SetUp() override;
    virtual void TearDown() override;

    struct file_desc
    {
        std::string filename;
        std::string md5;
        size_t length;
    };

    file_desc f1;
    file_desc f2;
    std::string local_file_for_download;
};

void FDSClientTest::SetUp()
{
    f1.filename = "test_file";
    f2.filename = "test_2";
    local_file_for_download = "local_download";

    // generate a test file
    {
        int lines = dsn_random32(1000, 2000);
        FILE *fp = fopen(f1.filename.c_str(), "wb");
        for (int i = 0; i < lines; ++i) {
            fprintf(fp, "%04d_this_is_a_simple_test_file\n", i);
        }
        fclose(fp);

        std::stringstream ss;
        pipe_execute((std::string("md5sum ") + f1.filename).c_str(), ss);
        ss >> f1.md5;
        // well, the string of each line in _test_file is 32
        f1.length = 32 * lines;
    }

    // generate another test file
    {
        int lines = dsn_random32(10, 20);
        FILE *fp = fopen(f2.filename.c_str(), "wb");
        for (int i = 0; i < lines; ++i) {
            fprintf(fp, "%04d_this_is_a_simple_test_file\n", i);
        }
        fclose(fp);

        std::stringstream ss;
        pipe_execute((std::string("md5sum ") + f2.filename).c_str(), ss);
        ss >> f2.md5;
        // well, the string of each line in _test_file is 32
        f2.length = 32 * lines;
    }
}

void FDSClientTest::TearDown() {}

DEFINE_TASK_CODE(lpc_btest, TASK_PRIORITY_HIGH, dsn::THREAD_POOL_DEFAULT)

TEST_F(FDSClientTest, test_basic_operation)
{
    const char *files[] = {"/fdstest1/test1/test1",
                           "/fdstest1/test1/test2",
                           "/fdstest1/test2/test1",
                           "/fdstest1/test2/test2",
                           "/fdstest2/test2",
                           "/fdstest3",
                           "/fds_rootfile",
                           nullptr};
    // ensure prefix_path is the prefix of some file in files
    std::string prefix_path = std::string("/fdstest1/test1");
    int total_files;

    std::shared_ptr<fds_service> s = std::make_shared<fds_service>();
    // server, access-key, access-secret, bucket_name
    std::vector<std::string> args = {"cnbj1-fds.api.xiaomi.net",
                                     "AK2TBDCN7PIPNKCWIX",
                                     "Fi1N30xEKZz0iLKflt0rGIZvFRtExx/ziDIC2gVy",
                                     "sunweijie-pegasus-test-bucket"};

    s->initialize(args);

    create_file_response cf_resp;
    delete_file_response df_resp;
    ls_response l_resp;
    upload_response u_resp;
    download_response d_resp;
    read_response r_resp;
    write_response w_resp;
    exist_response e_resp;
    remove_path_response rem_resp;

    auto entry_cmp = [](const ls_entry &entry1, const ls_entry &entry2) {
        return entry1.entry_name < entry2.entry_name;
    };
    auto entry_vec_eq = [](const std::vector<ls_entry> &entry_vec1,
                           const std::vector<ls_entry> &entry_vec2) {
        ASSERT_EQ(entry_vec1.size(), entry_vec2.size());
        for (unsigned int i = 0; i < entry_vec1.size(); ++i) {
            ASSERT_EQ(entry_vec1[i].entry_name, entry_vec2[i].entry_name);
            ASSERT_EQ(entry_vec1[i].is_directory, entry_vec2[i].is_directory)
                << "on index " << i << ", name " << entry_vec1[i].entry_name;
        }
    };

    // first clean all
    {
        std::cout << "clean all old files" << std::endl;
        for (int i = 0; files[i]; ++i) {
            std::cout << "delete file " << files[i] << std::endl;
            delete_file_response dr;
            s->delete_file(delete_file_request{files[i]},
                           lpc_btest,
                           [&dr](const delete_file_response &r) { dr = r; },
                           nullptr)
                ->wait();
            ASSERT_TRUE(dsn::ERR_OK == dr.err || dsn::ERR_OBJECT_NOT_FOUND == dr.err);
        }
    }

    // first upload all these files
    {
        std::cout << "Test upload files" << std::endl;
        for (total_files = 0; files[total_files]; ++total_files) {
            std::cout << "create and upload: " << files[total_files] << std::endl;
            s->create_file(create_file_request{std::string(files[total_files]), true},
                           lpc_btest,
                           [&cf_resp](const create_file_response &r) { cf_resp = r; },
                           nullptr)
                ->wait();
            ASSERT_EQ(cf_resp.err, dsn::ERR_OK);

            cf_resp.file_handle
                ->upload(upload_request{FDSClientTest::f1.filename},
                         lpc_btest,
                         [&u_resp](const upload_response &r) { u_resp = r; },
                         nullptr)
                ->wait();

            ASSERT_EQ(dsn::ERR_OK, u_resp.err);
            ASSERT_EQ(FDSClientTest::f1.length, cf_resp.file_handle->get_size());
            ASSERT_EQ(FDSClientTest::f1.md5, cf_resp.file_handle->get_md5sum());
        }

        // create a non-exist file for read
        {
            std::cout << "create a non-exist file for read: fds_hellworld" << std::endl;
            s->create_file(create_file_request{std::string("fds_helloworld"), false},
                           lpc_btest,
                           [&cf_resp](const create_file_response &r) { cf_resp = r; },
                           nullptr)
                ->wait();

            ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
            ASSERT_TRUE(cf_resp.file_handle->get_md5sum().empty());
        }

        // create an exist-file for write
        {
            std::cout << "create an exist file for write: " << files[total_files - 1] << std::endl;
            s->create_file(create_file_request{std::string(files[total_files - 1]), false},
                           lpc_btest,
                           [&cf_resp](const create_file_response &r) { cf_resp = r; },
                           nullptr)
                ->wait();

            ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
            ASSERT_EQ(FDSClientTest::f1.md5, cf_resp.file_handle->get_md5sum());

            cf_resp.file_handle
                ->upload(upload_request{FDSClientTest::f2.filename},
                         lpc_btest,
                         [&u_resp](const upload_response &r) { u_resp = r; },
                         nullptr)
                ->wait();

            ASSERT_EQ(dsn::ERR_OK, u_resp.err);
            ASSERT_EQ(FDSClientTest::f2.length, cf_resp.file_handle->get_size());
            ASSERT_EQ(FDSClientTest::f2.md5, cf_resp.file_handle->get_md5sum());

            // upload an non-exist local file
            cf_resp.file_handle
                ->upload(upload_request{"joke_file"},
                         lpc_btest,
                         [&u_resp](const upload_response &r) { u_resp = r; },
                         nullptr)
                ->wait();
            ASSERT_EQ(dsn::ERR_FILE_OPERATION_FAILED, u_resp.err);

            // upload an local file which we don't have read-permission
            cf_resp.file_handle
                ->upload(upload_request{"/root/.profile"},
                         lpc_btest,
                         [&u_resp](const upload_response &r) { u_resp = r; },
                         nullptr)
                ->wait();
            ASSERT_EQ(dsn::ERR_FILE_OPERATION_FAILED, u_resp.err);
        }
    }

    // then test exist
    {
        std::cout << "Test file that not exist" << std::endl;
        s->exist(exist_request{std::string("/fds_test1/file_do_not_exist")},
                 lpc_btest,
                 [&e_resp](const exist_response &resp) { e_resp = resp; },
                 nullptr)
            ->wait();
        ASSERT_EQ(e_resp.err, ERR_OBJECT_NOT_FOUND);

        std::cout << "Test file that exist" << std::endl;
        s->exist(exist_request{std::string(files[3])},
                 lpc_btest,
                 [&e_resp](const exist_response &resp) { e_resp = resp; },
                 nullptr)
            ->wait();
        ASSERT_EQ(e_resp.err, ERR_OK);

        std::cout << "Test path not exist" << std::endl;
        s->exist(exist_request{std::string("/fds_test1/path_do_not_exist")},
                 lpc_btest,
                 [&e_resp](const exist_response &resp) { e_resp = resp; },
                 nullptr)
            ->wait();
        ASSERT_EQ(e_resp.err, ERR_OBJECT_NOT_FOUND);

        std::cout << "Test path exist" << std::endl;
        s->exist(exist_request{std::string(prefix_path)},
                 lpc_btest,
                 [&e_resp](const exist_response &resp) { e_resp = resp; },
                 nullptr)
            ->wait();
        ASSERT_EQ(e_resp.err, ERR_OK);
    }

    // then test list files
    {
        std::cout << "test ls files" << std::endl;

        // list the root
        std::cout << "list the root" << std::endl;
        std::vector<ls_entry> root = {
            {"fdstest1", true}, {"fdstest2", true}, {"fdstest3", false}, {"fds_rootfile", false}};
        std::sort(root.begin(), root.end(), entry_cmp);

        s->list_dir(ls_request{"/"},
                    lpc_btest,
                    [&l_resp](const ls_response &resp) { l_resp = resp; },
                    nullptr)
            ->wait();
        ASSERT_EQ(dsn::ERR_OK, l_resp.err);
        std::sort(l_resp.entries->begin(), l_resp.entries->end(), entry_cmp);
        entry_vec_eq(root, *l_resp.entries);

        // list the fdstest1
        std::cout << "list the fdstest1" << std::endl;
        std::vector<ls_entry> fdstest1 = {{"test1", true}, {"test2", true}};
        std::sort(fdstest1.begin(), fdstest1.end(), entry_cmp);

        s->list_dir(ls_request{"/fdstest1"},
                    lpc_btest,
                    [&l_resp](const ls_response &resp) { l_resp = resp; },
                    nullptr)
            ->wait();
        std::sort(l_resp.entries->begin(), l_resp.entries->end(), entry_cmp);
        entry_vec_eq(fdstest1, *l_resp.entries);

        // list the fdstest1/test2
        std::cout << "list the fdstest1/test2" << std::endl;
        std::vector<ls_entry> fdstest1_slash_test2 = {{"test1", false}, {"test2", false}};
        std::sort(fdstest1_slash_test2.begin(), fdstest1_slash_test2.end(), entry_cmp);

        s->list_dir(ls_request{"/fdstest1/test2"},
                    lpc_btest,
                    [&l_resp](const ls_response &resp) { l_resp = resp; },
                    nullptr)
            ->wait();
        std::sort(l_resp.entries->begin(), l_resp.entries->end(), entry_cmp);
        entry_vec_eq(fdstest1_slash_test2, *l_resp.entries);

        // list a non-exist dir
        std::cout << "list a non-exist dir /fds_hehe" << std::endl;
        s->list_dir(ls_request{"/fds_hehe"},
                    lpc_btest,
                    [&l_resp](const ls_response &resp) { l_resp = resp; },
                    nullptr)
            ->wait();
        ASSERT_EQ(dsn::ERR_OBJECT_NOT_FOUND, l_resp.err);

        // list a regular file
        std::cout << "list a regular file /fds_rootfile" << std::endl;
        s->list_dir(ls_request{"/fds_rootfile"},
                    lpc_btest,
                    [&l_resp](const ls_response &resp) { l_resp = resp; },
                    nullptr)
            ->wait();
        ASSERT_EQ(dsn::ERR_INVALID_PARAMETERS, l_resp.err);
    }

    // then test download files
    {
        std::cout << "test download file, don't ignore metadata" << std::endl;
        for (int i = 0; i < total_files - 1; ++i) {
            std::cout << "test download " << files[i] << std::endl;
            s->create_file(create_file_request{files[i], false},
                           lpc_btest,
                           [&cf_resp](const create_file_response &resp) { cf_resp = resp; },
                           nullptr)
                ->wait();
            ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
            ASSERT_NE(nullptr, cf_resp.file_handle.get());
            ASSERT_EQ(f1.length, cf_resp.file_handle->get_size());
            ASSERT_EQ(f1.md5, cf_resp.file_handle->get_md5sum());

            cf_resp.file_handle
                ->download(download_request{local_file_for_download, 0, -1},
                           lpc_btest,
                           [&d_resp](const download_response &resp) { d_resp = resp; },
                           nullptr)
                ->wait();
            ASSERT_EQ(dsn::ERR_OK, d_resp.err);
            ASSERT_EQ(cf_resp.file_handle->get_size(), d_resp.downloaded_size);
            file_eq_compare(f1.filename, local_file_for_download);
        }

        std::cout << "test download file, ignore metadata: " << files[total_files - 1] << std::endl;
        s->create_file(create_file_request{files[total_files - 1], true},
                       lpc_btest,
                       [&cf_resp](const create_file_response &resp) { cf_resp = resp; },
                       nullptr)
            ->wait();

        ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
        ASSERT_NE(nullptr, cf_resp.file_handle.get());
        ASSERT_TRUE(cf_resp.file_handle->get_md5sum().empty());

        cf_resp.file_handle
            ->download(download_request{local_file_for_download, 0, -1},
                       lpc_btest,
                       [&d_resp](const download_response &resp) { d_resp = resp; },
                       nullptr)
            ->wait();
        ASSERT_EQ(f2.length, cf_resp.file_handle->get_size());
        ASSERT_EQ(f2.md5, cf_resp.file_handle->get_md5sum());
        file_eq_compare(f2.filename, local_file_for_download);

        std::cout << "test partitial download " << std::endl;
        cf_resp.file_handle
            ->download(download_request{local_file_for_download, 32, 32},
                       lpc_btest,
                       [&d_resp](const download_response &resp) { d_resp = resp; },
                       nullptr)
            ->wait();

        ASSERT_EQ(dsn::ERR_OK, d_resp.err);
        ASSERT_EQ(32, d_resp.downloaded_size);
        {
            std::shared_ptr<FILE> f(fopen("tmp_generate", "wb"), [](FILE *p) { fclose(p); });
            fprintf(f.get(), "%04d_this_is_a_simple_test_file\n", 1);
        }
        file_eq_compare(std::string("tmp_generate"), local_file_for_download);
    }

    // try to read a non-exist file
    {
        std::cout << "test try to read non-exist file" << std::endl;
        s->create_file(create_file_request{"fds_hellword", true},
                       lpc_btest,
                       [&cf_resp](const create_file_response &r) { cf_resp = r; },
                       nullptr)
            ->wait();

        ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
        ASSERT_NE(nullptr, cf_resp.file_handle.get());
        ASSERT_TRUE(cf_resp.file_handle->get_md5sum().empty());

        cf_resp.file_handle
            ->read(read_request{0, -1},
                   lpc_btest,
                   [&r_resp](const read_response &r) { r_resp = r; },
                   nullptr)
            ->wait();
        ASSERT_EQ(dsn::ERR_OBJECT_NOT_FOUND, r_resp.err);

        // now file handle has been synced from remote
        cf_resp.file_handle
            ->download(download_request{"local_file", 0, -1},
                       lpc_btest,
                       [&d_resp](const download_response &r) { d_resp = r; },
                       nullptr)
            ->wait();
        ASSERT_EQ(dsn::ERR_OBJECT_NOT_FOUND, d_resp.err);
        // so we expect the file doesn't create
        ASSERT_FALSE(dsn::utils::filesystem::file_exists("local_file"));
    }

    // try to download to a path where we can't create the file
    {
        std::cout << "test try to download to a path where we can't create the file" << std::endl;
        s->create_file(create_file_request{files[0], false},
                       lpc_btest,
                       [&cf_resp](const create_file_response &r) { cf_resp = r; },
                       nullptr)
            ->wait();

        ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
        ASSERT_NE(nullptr, cf_resp.file_handle.get());
        ASSERT_EQ(cf_resp.file_handle->get_size(), f1.length);
        ASSERT_EQ(cf_resp.file_handle->get_md5sum(), f1.md5);

        cf_resp.file_handle
            ->download(download_request{"test_dir/test_file.txt", 0, -1},
                       lpc_btest,
                       [&d_resp](const download_response &r) { d_resp = r; },
                       nullptr)
            ->wait();

        ASSERT_EQ(dsn::ERR_FILE_OPERATION_FAILED, d_resp.err);
        ASSERT_EQ(0, d_resp.downloaded_size);

        cf_resp.file_handle
            ->download(download_request{"/root/.profile", 0, -1},
                       lpc_btest,
                       [&d_resp](const download_response &r) { d_resp = r; },
                       nullptr)
            ->wait();

        ASSERT_EQ(dsn::ERR_FILE_OPERATION_FAILED, d_resp.err);
        ASSERT_EQ(0, d_resp.downloaded_size);
    }

    // try to do write/read
    {
        std::cout << "test read write operation" << std::endl;
        s->create_file(create_file_request{files[0], false},
                       lpc_btest,
                       [&cf_resp](const create_file_response &r) { cf_resp = r; },
                       nullptr)
            ->wait();

        ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
        ASSERT_NE(nullptr, cf_resp.file_handle.get());
        ASSERT_EQ(cf_resp.file_handle->get_size(), f1.length);
        ASSERT_EQ(cf_resp.file_handle->get_md5sum(), f1.md5);

        const char *test_buffer = "1234567890qwertyuiopasdfghjklzxcvbnm";
        int length = strlen(test_buffer);
        dsn::blob bb(test_buffer, 0, length);

        cf_resp.file_handle
            ->write(write_request{bb},
                    lpc_btest,
                    [&w_resp](const write_response &w) { w_resp = w; },
                    nullptr)
            ->wait();

        ASSERT_EQ(dsn::ERR_OK, w_resp.err);
        ASSERT_EQ(length, w_resp.written_size);
        ASSERT_EQ(length, cf_resp.file_handle->get_size());
        ASSERT_NE(f1.md5, cf_resp.file_handle->get_md5sum());

        std::cout << "test read just written value" << std::endl;
        cf_resp.file_handle
            ->read(read_request{0, -1},
                   lpc_btest,
                   [&r_resp](const read_response &r) { r_resp = r; },
                   nullptr)
            ->wait();

        ASSERT_EQ(dsn::ERR_OK, r_resp.err);
        ASSERT_EQ(length, r_resp.buffer.length());
        ASSERT_EQ(0, memcmp(r_resp.buffer.data(), test_buffer, length));

        // partitial read
        cf_resp.file_handle
            ->read(read_request{5, 10},
                   lpc_btest,
                   [&r_resp](const read_response &r) { r_resp = r; },
                   nullptr)
            ->wait();

        ASSERT_EQ(dsn::ERR_OK, r_resp.err);
        ASSERT_EQ(10, r_resp.buffer.length());
        ASSERT_EQ(0, memcmp(r_resp.buffer.data(), test_buffer + 5, 10));
    }

    // then test delete files
    {
        std::cout << "test delete files" << std::endl;
        for (int i = 0; i < total_files; ++i) {
            std::cout << "test delete file " << files[i] << std::endl;
            s->delete_file(delete_file_request{files[i]},
                           lpc_btest,
                           [&df_resp](const delete_file_response &resp) { df_resp = resp; },
                           nullptr)
                ->wait();
            ASSERT_EQ(df_resp.err, dsn::ERR_OK);

            s->create_file(create_file_request{files[i], false},
                           lpc_btest,
                           [&cf_resp](const create_file_response &resp) { cf_resp = resp; },
                           nullptr)
                ->wait();

            ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
            ASSERT_TRUE(cf_resp.file_handle->get_md5sum().empty());
        }
    }

    // then test remove path
    {
        // first re-upload all the files
        for (total_files = 0; files[total_files]; ++total_files) {
            std::cout << "create and upload: " << files[total_files] << std::endl;
            s->create_file(create_file_request{std::string(files[total_files]), true},
                           lpc_btest,
                           [&cf_resp](const create_file_response &r) { cf_resp = r; },
                           nullptr)
                ->wait();
            ASSERT_EQ(cf_resp.err, dsn::ERR_OK);

            cf_resp.file_handle
                ->upload(upload_request{FDSClientTest::f1.filename},
                         lpc_btest,
                         [&u_resp](const upload_response &r) { u_resp = r; },
                         nullptr)
                ->wait();

            ASSERT_EQ(dsn::ERR_OK, u_resp.err);
            ASSERT_EQ(FDSClientTest::f1.length, cf_resp.file_handle->get_size());
            ASSERT_EQ(FDSClientTest::f1.md5, cf_resp.file_handle->get_md5sum());
        }

        // then test remove_path
        {
            std::cout << "Test remove non-empty path with recusive = false" << std::endl;
            s->remove_path(remove_path_request{std::string(prefix_path), false},
                           lpc_btest,
                           [&rem_resp](const remove_path_response &resp) { rem_resp = resp; },
                           nullptr)
                ->wait();
            ASSERT_EQ(rem_resp.err, ERR_DIR_NOT_EMPTY);

            std::cout << "Test remove non-empty path with recusive = true" << std::endl;
            s->remove_path(remove_path_request{std::string(prefix_path), true},
                           lpc_btest,
                           [&rem_resp](const remove_path_response &resp) { rem_resp = resp; },
                           nullptr)
                ->wait();
            ASSERT_EQ(rem_resp.err, ERR_OK);

            std::cout << "Test remove a common path" << std::endl;
            for (total_files = 0; files[total_files]; total_files++) {
                std::string filename = files[total_files];
                // file under prefix_path already removed
                if (filename.find(prefix_path) == std::string::npos) {
                    // remove a single file with recusive = true/false
                    bool recursive = ((total_files % 2) == 0);
                    s->remove_path(
                         remove_path_request{std::string(files[total_files]), recursive},
                         lpc_btest,
                         [&rem_resp](const remove_path_response &resp) { rem_resp = resp; },
                         nullptr)
                        ->wait();
                    ASSERT_EQ(rem_resp.err, ERR_OK);
                }
                s->create_file(create_file_request{files[total_files], false},
                               lpc_btest,
                               [&cf_resp](const create_file_response &resp) { cf_resp = resp; },
                               nullptr)
                    ->wait();

                ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
                ASSERT_TRUE(cf_resp.file_handle->get_md5sum().empty());
            }
        }

        // test list_dir that total file/dir count greater than 1000
        {
            int total_file_cnt = 30;
            std::cout << "upload " << total_file_cnt << " to fds server" << std::endl;
            std::string prefix = "/fdstest_prefix";
            std::string file_name_prefix = "file_";
            std::vector<ls_entry> t_entries;

            // first clean in case last test failed
            std::cout << "first clean the dir in case old test failed" << std::endl;
            s->remove_path(remove_path_request{std::string(prefix), true},
                           lpc_btest,
                           [this, &rem_resp](const remove_path_response &resp) { rem_resp = resp; },
                           nullptr)
                ->wait();

            // generate total_file_cnt files
            std::cout << "then start to upload files" << std::endl;
            for (int i = 1; i <= total_file_cnt; i++) {
                std::string filename = file_name_prefix + std::to_string(i);
                t_entries.emplace_back(ls_entry{std::string(filename), false});

                s->create_file(create_file_request{std::string(prefix + "/" + filename), true},
                               lpc_btest,
                               [&cf_resp](const create_file_response &r) { cf_resp = r; },
                               nullptr)
                    ->wait();
                ASSERT_EQ(cf_resp.err, dsn::ERR_OK);

                cf_resp.file_handle
                    ->upload(upload_request{FDSClientTest::f1.filename},
                             lpc_btest,
                             [&u_resp](const upload_response &r) { u_resp = r; },
                             nullptr)
                    ->wait();

                ASSERT_EQ(dsn::ERR_OK, u_resp.err);
                ASSERT_EQ(FDSClientTest::f1.length, cf_resp.file_handle->get_size());
                ASSERT_EQ(FDSClientTest::f1.md5, cf_resp.file_handle->get_md5sum());
            }
            l_resp.entries->clear();
            s->list_dir(ls_request{prefix},
                        lpc_btest,
                        [this, &l_resp](const ls_response &resp) { l_resp = resp; },
                        nullptr)
                ->wait();
            ASSERT_EQ(l_resp.err, ERR_OK);
            ASSERT_EQ(l_resp.entries->size(), total_file_cnt);
            std::sort(l_resp.entries->begin(), l_resp.entries->end(), entry_cmp);
            std::sort(t_entries.begin(), t_entries.end(), entry_cmp);
            entry_vec_eq(t_entries, *l_resp.entries);

            // then remove all the file, using remove_path
            std::cout << "then remove all the files, using remove path" << std::endl;
            s->remove_path(remove_path_request{std::string(prefix), true},
                           lpc_btest,
                           [this, &rem_resp](const remove_path_response &resp) { rem_resp = resp; },
                           nullptr)
                ->wait();
            ASSERT_EQ(rem_resp.err, ERR_OK);
            s->exist(exist_request{std::string(prefix)},
                     lpc_btest,
                     [this, &e_resp](const exist_response &resp) { e_resp = resp; },
                     nullptr)
                ->wait();
            ASSERT_EQ(e_resp.err, ERR_OBJECT_NOT_FOUND);
        }
    }
}

static void
generate_file(const char *filename, unsigned long long file_size, char *block, unsigned block_size)
{
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    ASSERT_TRUE(fd > 0) << strerror(errno) << std::endl;
    for (unsigned long long i = 0; i < file_size;) {
        int batch_size = (file_size - i);
        if (batch_size > block_size)
            batch_size = block_size;
        i += batch_size;

        for (int j = 0; j < batch_size; ++j) {
            block[j] = (char)dsn_random32(0, 255);
        }
        write(fd, block, batch_size);
    }
    close(fd);
}

TEST_F(FDSClientTest, test_concurrent_upload_download)
{
    char block[1024];
    const char *str = "test_str";
    for (int i = 0; i < 128; ++i) {
        memcpy(block + i * 8, str, 8);
    }

    std::shared_ptr<fds_service> _service = std::make_shared<fds_service>();
    std::vector<std::string> init_str = {"cnbj1-fds.api.xiaomi.net",
                                         "AK2TBDCN7PIPNKCWIX",
                                         "Fi1N30xEKZz0iLKflt0rGIZvFRtExx/ziDIC2gVy",
                                         "sunweijie-pegasus-test-bucket"};

    _service->initialize(init_str);

    int total_files = dsn_config_get_value_uint64("fds_concurrent_test", "total_files", 64, "");
    unsigned long min_size = dsn_config_get_value_uint64("fds_concurrent_test", "min_size", 64, "");
    unsigned long max_size = dsn_config_get_value_uint64("fds_concurrent_test", "min_size", 64, "");

    std::vector<std::string> filenames;
    filenames.reserve(total_files);
    std::vector<unsigned long> filesize;
    filesize.reserve(total_files);
    std::vector<std::string> md5;
    md5.reserve(total_files);

    for (int i = 0; i < total_files; ++i) {
        char index[64];
        snprintf(index, 64, "%04d", i);
        unsigned long random_size = dsn_random64(min_size, max_size);
        std::string filename = "randomfile" + std::string(index);
        filenames.push_back(filename);
        filesize.push_back(random_size);
        generate_file(filename.c_str(), random_size, block, 1024);

        std::string md5result;
        dsn::utils::filesystem::md5sum(filename, md5result);
        md5.push_back(md5result);
    }

    printf("start delete phase\n");
    {
        for (unsigned int i = 0; i < total_files; ++i) {
            _service
                ->delete_file(delete_file_request{filenames[i]},
                              lpc_btest,
                              [i, &filenames](const delete_file_response &resp) {
                                  printf("file %s delete finished, err(%s)\n",
                                         filenames[i].c_str(),
                                         resp.err.to_string());
                              },
                              nullptr)
                ->wait();
        }
    }

    printf("start upload phase\n");
    {
        std::vector<block_file_ptr> block_files;
        for (unsigned int i = 0; i < total_files; ++i) {
            create_file_response cf_resp;
            _service
                ->create_file(create_file_request{filenames[i], true},
                              lpc_btest,
                              [&cf_resp](const create_file_response &r) { cf_resp = r; },
                              nullptr)
                ->wait();
            ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
            ASSERT_NE(nullptr, cf_resp.file_handle.get());
            block_files.push_back(cf_resp.file_handle);
        }

        std::vector<dsn::task_ptr> callbacks;
        for (unsigned int i = 0; i < total_files; ++i) {
            block_file_ptr p = block_files[i];
            dsn::task_ptr t =
                p->upload(upload_request{filenames[i]},
                          lpc_btest,
                          [p, &filenames, &filesize, &md5, i](const upload_response &ur) {
                              printf("file %s upload finished\n", filenames[i].c_str());
                              ASSERT_EQ(dsn::ERR_OK, ur.err);
                              ASSERT_EQ(filesize[i], ur.uploaded_size);
                              ASSERT_EQ(filesize[i], p->get_size());
                              ASSERT_EQ(md5[i], p->get_md5sum());
                          });
            callbacks.push_back(t);
        }

        for (unsigned int i = 0; i < total_files; ++i) {
            callbacks[i]->wait();
        }
    }

    printf("start download phase\n");
    {
        std::vector<block_file_ptr> block_files;
        for (unsigned int i = 0; i < total_files; ++i) {
            create_file_response cf_resp;
            _service
                ->create_file(create_file_request{filenames[i], true},
                              lpc_btest,
                              [&cf_resp](const create_file_response &r) { cf_resp = r; },
                              nullptr)
                ->wait();
            ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
            ASSERT_NE(nullptr, cf_resp.file_handle.get());
            block_files.push_back(cf_resp.file_handle);
        }

        std::vector<dsn::task_ptr> callbacks;
        for (unsigned int i = 0; i < total_files; ++i) {
            block_file_ptr p = block_files[i];
            dsn::task_ptr t =
                p->download(download_request{filenames[i] + ".b"},
                            lpc_btest,
                            [&filenames, &filesize, &md5, i, p](const download_response &dr) {
                                printf("file %s download finished\n", filenames[i].c_str());
                                ASSERT_EQ(dsn::ERR_OK, dr.err);
                                ASSERT_EQ(filesize[i], dr.downloaded_size);
                                ASSERT_EQ(filesize[i], p->get_size());
                                ASSERT_EQ(md5[i], p->get_md5sum());
                            });
            callbacks.push_back(t);
        }

        for (unsigned int i = 0; i < total_files; ++i) {
            callbacks[i]->wait();
        }
    }

    printf("start cleanup phase\n");
    {
        for (unsigned int i = 0; i < total_files; ++i) {
            _service
                ->delete_file(delete_file_request{filenames[i]},
                              lpc_btest,
                              [i, &filenames](const delete_file_response &resp) {
                                  printf("file %s delete finished, err(%s)\n",
                                         filenames[i].c_str(),
                                         resp.err.to_string());
                              },
                              nullptr)
                ->wait();
        }
    }
}

// TEST_F(FDSClientTest, test_max_file_size)
//{
//    char block[1024];
//    const char *str = "test_str";
//    for (int i = 0; i < 128; ++i) {
//        memcpy(block + i * 8, str, 8);
//    }
//
//    std::shared_ptr<fds_service> _service = std::make_shared<fds_service>();
//    std::vector<std::string> init_str = {"cnbj1-fds.api.xiaomi.net",
//                                         "AK2TBDCN7PIPNKCWIX",
//                                         "Fi1N30xEKZz0iLKflt0rGIZvFRtExx/ziDIC2gVy",
//                                         "sunweijie-pegasus-test-bucket"};
//
//    _service->initialize(init_str);
//
//    create_file_response cf_resp;
//    _service
//        ->create_file(create_file_request{"upload_test", true},
//                      lpc_btest,
//                      [&cf_resp](const create_file_response &r) { cf_resp = r; },
//                      nullptr)
//        ->wait();
//
//    ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
//    ASSERT_NE(nullptr, cf_resp.file_handle.get());
//
//    unsigned long prev_size = 32 * 1024;
//    unsigned long new_size = prev_size * 2;
//    unsigned long ceiling = 0;
//
//    while (new_size > prev_size) {
//        generate_file("test_local_file", new_size, block, 1024);
//        upload_response u_resp;
//
//        uint64_t start = dsn_now_ms();
//        cf_resp.file_handle
//            ->upload(upload_request{"test_local_file"},
//                     lpc_btest,
//                     [&u_resp](const upload_response &r) { u_resp = r; },
//                     nullptr)
//            ->wait();
//        uint64_t end = dsn_now_ms();
//
//        if (u_resp.err == dsn::ERR_OK && u_resp.uploaded_size == new_size) {
//            printf("upload a file with size %lf MB succeed, time consume %lf sec\n",
//                   new_size / (1024.0 * 1024.0),
//                   (end - start) / 1000.0);
//            if (ceiling == 0) {
//                prev_size = new_size;
//                new_size *= 2;
//            } else {
//                prev_size = new_size;
//                new_size = (ceiling + prev_size) / 2;
//            }
//        } else {
//            printf("upload a file with size %lf MB failed, time consume %lf sec\n",
//                   new_size / (1024.0 * 1024.0),
//                   (end - start) / 1000.0);
//            ceiling = new_size;
//            new_size = (ceiling + prev_size) / 2;
//        }
//    }
//}
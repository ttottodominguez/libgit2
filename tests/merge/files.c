#include "clar_libgit2.h"
#include "git2/repository.h"
#include "git2/merge.h"
#include "buffer.h"
#include "merge.h"
#include "merge_helpers.h"
#include "refs.h"
#include "fileops.h"

#define TEST_REPO_PATH "merge-resolve"
#define TEST_INDEX_PATH TEST_REPO_PATH "/.git/index"

static git_repository *repo;
static git_index *repo_index;

// Fixture setup and teardown
void test_merge_files__initialize(void)
{
	git_config *cfg;

	repo = cl_git_sandbox_init(TEST_REPO_PATH);
	git_repository_index(&repo_index, repo);

	/* Ensure that the user's merge.conflictstyle doesn't interfere */
	cl_git_pass(git_repository_config(&cfg, repo));
	cl_git_pass(git_config_set_string(cfg, "merge.conflictstyle", "merge"));
	git_config_free(cfg);
}

void test_merge_files__cleanup(void)
{
	git_index_free(repo_index);
	cl_git_sandbox_cleanup();
}

void test_merge_files__automerge_from_files(void)
{
	git_merge_file_result result = GIT_MERGE_FILE_RESULT_INIT;
	git_merge_file_options opts = GIT_MERGE_FILE_OPTIONS_INIT;

	cl_git_rewritefile("file1.txt", "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
	cl_git_rewritefile("file2.txt", "Zero\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
	cl_git_rewritefile("file3.txt", "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\nTen\n");

	cl_git_pass(git_merge_file(&result, "file1.txt", "file2.txt", "file3.txt", &opts));

	cl_assert_equal_i(1, result.automergeable);

	cl_assert_equal_s(NULL, result.path);
	cl_assert_equal_i(0100644, result.mode);

	cl_assert_equal_s("Zero\n1\n2\n3\n4\n5\n6\n7\n8\n9\nTen\n", result.data);

	git_merge_file_result_free(&result);
}

void test_merge_files__automerge_from_index(void)
{
	git_merge_file_result result = GIT_MERGE_FILE_RESULT_INIT;
	git_merge_file_options opts = GIT_MERGE_FILE_OPTIONS_INIT;
	git_index_entry ancestor, ours, theirs;

	git_oid_fromstr(&ancestor.id, "6212c31dab5e482247d7977e4f0dd3601decf13b");
	ancestor.path = "automergeable.txt";
	ancestor.mode = 0100644;

	git_oid_fromstr(&ours.id, "ee3fa1b8c00aff7fe02065fdb50864bb0d932ccf");
	ours.path = "automergeable.txt";
	ours.mode = 0100755;

	git_oid_fromstr(&theirs.id, "058541fc37114bfc1dddf6bd6bffc7fae5c2e6fe");
	theirs.path = "newname.txt";
	theirs.mode = 0100644;

	cl_git_pass(git_merge_file_from_index(&result, repo,
		&ancestor, &ours, &theirs, &opts));

	cl_assert_equal_i(1, result.automergeable);

	cl_assert_equal_s("newname.txt", result.path);
	cl_assert_equal_i(0100755, result.mode);

	cl_assert_equal_s(AUTOMERGEABLE_MERGED_FILE, result.data);
	cl_assert_equal_i(strlen(AUTOMERGEABLE_MERGED_FILE), result.len);

	git_merge_file_result_free(&result);
}

void test_merge_files__automerge_filtered(void)
{
}

void test_merge_files__conflict_merge(void)
{
}

void test_merge_files__conflict_diff3(void)
{
}

void test_merge_files__conflict_filtered(void)
{
}


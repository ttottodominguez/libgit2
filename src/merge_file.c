/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"
#include "repository.h"
#include "merge_file.h"
#include "posix.h"
#include "fileops.h"
#include "index.h"

#include "git2/repository.h"
#include "git2/object.h"
#include "git2/index.h"

#include "xdiff/xdiff.h"

#define GIT_MERGE_FILE_SIDE_EXISTS(X)	((X)->mode != 0)

GIT_INLINE(const char *) merge_file_best_path(
	const git_merge_file_input *ancestor,
	const git_merge_file_input *ours,
	const git_merge_file_input *theirs)
{
	if (!GIT_MERGE_FILE_SIDE_EXISTS(ancestor)) {
		if (strcmp(ours->path, theirs->path) == 0)
			return ours->path;

		return NULL;
	}

	if (strcmp(ancestor->path, ours->path) == 0)
		return theirs->path;
	else if(strcmp(ancestor->path, theirs->path) == 0)
		return ours->path;

	return NULL;
}

GIT_INLINE(int) merge_file_best_mode(
	const git_merge_file_input *ancestor,
	const git_merge_file_input *ours,
	const git_merge_file_input *theirs)
{
	/*
	 * If ancestor didn't exist and either ours or theirs is executable,
	 * assume executable.  Otherwise, if any mode changed from the ancestor,
	 * use that one.
	 */
	if (!GIT_MERGE_FILE_SIDE_EXISTS(ancestor)) {
		if (ours->mode == GIT_FILEMODE_BLOB_EXECUTABLE ||
			theirs->mode == GIT_FILEMODE_BLOB_EXECUTABLE)
			return GIT_FILEMODE_BLOB_EXECUTABLE;

		return GIT_FILEMODE_BLOB;
	}

	if (ancestor->mode == ours->mode)
		return theirs->mode;
	else if(ancestor->mode == theirs->mode)
		return ours->mode;

	return 0;
}

int git_merge_file__input_from_file(
	git_merge_file_input *input,
	const char *path)
{
	git_buf data_buf = GIT_BUF_INIT;
	struct stat st;
	int error = 0;

	if ((error = p_stat(path, &st)) < 0) {
		giterr_set(GITERR_MERGE, "Could not read '%s'", path);
		goto done;
	}

	if ((error = git_futils_readbuffer(&data_buf, path)) < 0)
		goto done;

	if ((input->path = git__strdup(path)) == NULL) {
		error = -1;
		goto done;
	}

	input->mode = git_index__create_mode(st.st_mode);

	input->mmfile.size = data_buf.size;
	input->mmfile.ptr = data_buf.ptr;

	git_buf_detach(&data_buf);

	if (input->label == NULL)
		input->label = input->path;

done:
	if (error)
		git_buf_free(&data_buf);

	return error;
}

int git_merge_file__input_from_index_entry(
	git_merge_file_input *input,
	git_repository *repo,
	const git_index_entry *entry)
{
	git_odb *odb = NULL;
	int error = 0;

	assert(input && repo && entry);

	if (entry->mode == 0)
		return 0;

	if ((error = git_repository_odb(&odb, repo)) < 0 ||
		(error = git_odb_read(&input->odb_object, odb, &entry->id)) < 0)
		goto done;

	if ((input->path = git__strdup(entry->path)) == NULL) {
		error = -1;
		goto done;
	}

	input->mode = entry->mode;
	input->mmfile.size = git_odb_object_size(input->odb_object);
	input->mmfile.ptr = (char *)git_odb_object_data(input->odb_object);

	if (input->label == NULL)
		input->label = input->path;

done:
	git_odb_free(odb);

	return error;
}

int git_merge_file__input_from_diff_file(
	git_merge_file_input *input,
	git_repository *repo,
	const git_diff_file *file)
{
	git_odb *odb = NULL;
	int error = 0;

	assert(input && repo && file);

	if (file->mode == 0)
		return 0;

	if ((error = git_repository_odb(&odb, repo)) < 0 ||
		(error = git_odb_read(&input->odb_object, odb, &file->id)) < 0)
		goto done;

	if ((input->path = git__strdup(file->path)) == NULL) {
		error = -1;
		goto done;
	}

	input->mode = file->mode;
	input->mmfile.size = git_odb_object_size(input->odb_object);
	input->mmfile.ptr = (char *)git_odb_object_data(input->odb_object);

	if (input->label == NULL)
		input->label = input->path;

done:
	git_odb_free(odb);

	return error;
}

int git_merge_file__from_inputs(
	git_merge_file_result *out,
	git_merge_file_input *ancestor,
	git_merge_file_input *ours,
	git_merge_file_input *theirs,
	git_merge_file_favor_t favor,
	git_merge_file_flags_t flags)
{
	xmparam_t xmparam;
	mmbuffer_t mmbuffer;
	int xdl_result;
	int error = 0;

	assert(out && ancestor && ours && theirs);

	memset(out, 0x0, sizeof(git_merge_file_result));

	if (!GIT_MERGE_FILE_SIDE_EXISTS(ours) || !GIT_MERGE_FILE_SIDE_EXISTS(theirs))
		return 0;

	memset(&xmparam, 0x0, sizeof(xmparam_t));
	xmparam.ancestor = ancestor->label;
	xmparam.file1 = ours->label;
	xmparam.file2 = theirs->label;

	out->path = merge_file_best_path(ancestor, ours, theirs);
	out->mode = merge_file_best_mode(ancestor, ours, theirs);

	if (favor == GIT_MERGE_FILE_FAVOR_OURS)
		xmparam.favor = XDL_MERGE_FAVOR_OURS;
	else if (favor == GIT_MERGE_FILE_FAVOR_THEIRS)
		xmparam.favor = XDL_MERGE_FAVOR_THEIRS;
	else if (favor == GIT_MERGE_FILE_FAVOR_UNION)
		xmparam.favor = XDL_MERGE_FAVOR_UNION;

	xmparam.level = (flags & GIT_MERGE_FILE_SIMPLIFY_ALNUM) ?
		XDL_MERGE_ZEALOUS_ALNUM : XDL_MERGE_ZEALOUS;

	if (flags & GIT_MERGE_FILE_STYLE_DIFF3)
		xmparam.style = XDL_MERGE_DIFF3;

	if ((xdl_result = xdl_merge(&ancestor->mmfile, &ours->mmfile,
		&theirs->mmfile, &xmparam, &mmbuffer)) < 0) {
		giterr_set(GITERR_MERGE, "Failed to merge files.");
		error = -1;
		goto done;
	}

	out->automergeable = (xdl_result == 0);
	out->data = (unsigned char *)mmbuffer.ptr;
	out->len = mmbuffer.size;

done:
	return error;
}

int git_merge_file(
	git_merge_file_result *result,
	const char *ancestor_path,
	const char *our_path,
	const char *their_path,
	const git_merge_file_options *opts)
{
	git_merge_file_input ancestor_input = {0},
		our_input = {0},
		their_input = {0};
	git_merge_file_flags_t flags = GIT_MERGE_FILE_DEFAULT;
	int error = 0;

	if (opts) {
		ancestor_input.label = opts->ancestor_label;
		our_input.label = opts->our_label;
		their_input.label = opts->their_label;

		flags = opts->flags;
	}

	if ((error = git_merge_file__input_from_file(&ancestor_input, ancestor_path)) < 0 ||
		(error = git_merge_file__input_from_file(&our_input, our_path)) < 0 ||
		(error = git_merge_file__input_from_file(&their_input, their_path)) < 0)
		goto done;

	if ((error = git_merge_file__from_inputs(result, 
		&ancestor_input,
		&our_input,
		&their_input,
		GIT_MERGE_FILE_FAVOR_NORMAL,
		flags)) < 0)
		goto done;

	if (result->path && (result->path = git__strdup(result->path)) == NULL)
		error = -1;

done:
	git_merge_file__input_free(&ancestor_input);
	git_merge_file__input_free(&our_input);
	git_merge_file__input_free(&their_input);

	return error;
}

int git_merge_file_from_index(
	git_merge_file_result *result,
	git_repository *repo,
	const git_index_entry *ancestor,
	const git_index_entry *ours,
	const git_index_entry *theirs,
	const git_merge_file_options *opts)
{
	git_merge_file_input ancestor_input = {0},
		our_input = {0},
		their_input = {0};
	git_merge_file_flags_t flags = GIT_MERGE_FILE_DEFAULT;
	int error = 0;

	if ((error = git_merge_file__input_from_index_entry(&ancestor_input, repo, ancestor)) < 0 ||
		(error = git_merge_file__input_from_index_entry(&our_input, repo, ours)) < 0 ||
		(error = git_merge_file__input_from_index_entry(&their_input, repo, theirs)) < 0)
		goto done;

	if (opts) {
		ancestor_input.label = opts->ancestor_label;
		our_input.label = opts->our_label;
		their_input.label = opts->their_label;

		flags = opts->flags;
	}

	if ((error = git_merge_file__from_inputs(result, 
		&ancestor_input,
		&our_input,
		&their_input,
		GIT_MERGE_FILE_FAVOR_NORMAL,
		flags)) < 0)
		goto done;

	if (result->path && (result->path = git__strdup(result->path)) == NULL)
		error = -1;

done:
	git_merge_file__input_free(&ancestor_input);
	git_merge_file__input_free(&our_input);
	git_merge_file__input_free(&their_input);

	return error;
}

void git_merge_file_result_free(git_merge_file_result *result)
{
	if (result == NULL)
		return;

	/* xdiff uses malloc() not git_malloc, so we use free(), not git_free() */
	if (result->data != NULL)
		free(result->data);
}


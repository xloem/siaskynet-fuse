#include "Fuse.h"
#include "Fuse-impl.h"

#include "siaskynet.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

class SiaSkynetFS : public Fusepp::Fuse<SiaSkynetFS>
{
public:
	SiaSkynetFS(bool readonly = false, size_t rewriting_threshold = 4096, std::string initial_skylink = {});

	int fuse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
	int fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
	int fuse_open(const char *path, struct fuse_file_info *fi);
	int fuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
	int fuse_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info * fi);

	static int getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
	{ return this_()->fuse_getattr(path, stbuf, fi); }
	static int readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
	{ return this_()->fuse_readdir(path, buf, filler, offset, fi, flags); }
	static int open(const char *path, struct fuse_file_info *fi)
	{ return this_()->fuse_open(path, fi); }
	static int read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
	{ return this_()->fuse_read(path, buf, size, offset, fi); }
	static int write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info * fi)
	{ return this_()->fuse_write(path, buf, size, offset, fi); }

	void sync();
	std::string skylink();

private:
	static sia::skynet skynet;

	bool readonly;
	size_t rewriting_threshold;

	/*
		So we'll want a structure for a file entry, and a set of responses.

		each file entry refers to a subfile within a response.
			some responses are entire files.  some are lists of files.
			each response has at least 1 metadata subfile.  so we can refer to a subfile within a response.
	*/

	struct skydir {
		std::shared_ptr<sia::skynet::response> skynet;
		sia::skynet::response::subfile & file;
		std::shared_ptr<skydir> parent;
	};

	skydir & get(std::string const & path);

	std::mutex tree_lock;
	std::unordered_map<std::string, skydir> tree;
};

#include "cpr/util.h"

SiaSkynetFS::SiaSkynetFS(bool readonly, size_t rewriting_threshold, std::string initial_skylink)
: readonly(readonly), rewriting_threshold(rewriting_threshold)
{
	if (!initial_skylink.size()) {
		initial_skylink = skynet.upload("/", {});
	}

	std::shared_ptr<sia::skynet::response> root_response(new sia::skynet::response(skynet.download(initial_skylink)));
	tree.emplace(std::string("/"), skydir{root_response, root_response->metadata});
}

std::string SiaSkynetFS::skylink()
{
	return get("/").skynet->skylink;
}

#include <iostream> // DEBUG
SiaSkynetFS::skydir & SiaSkynetFS::get(std::string const & path)
{
	auto result = tree.find(path);
	if (result != tree.end()) {
		return result->second;
	}

	if (readonly) {
		throw -EACCES;
	}

	std::cerr << "NEW PATH: " << path << std::endl;

	// not readonly, and path doesn't exist.
	auto path_components = cpr::util::split(path, '/');
	auto subpath = path_components.back();
	auto superpath = path;
	superpath.resize(path.size() - subpath.size());

	auto parent = &get(superpath);

	std::lock_guard<std::mutex> lock(tree_lock);

	// parent will need to be updated to add us.  the new one won't have a skylink for now.
	// I guess we'll start by including us entirely, and update to a link if the total size surpasses the threshold.

	// when we copy the parent response we'll want a link to the same subfile within it, so as to replace it

	sia::skynet::response new_skynet = *(parent->skynet);
	new_skynet.skylink.clear();

	// TODO: sub-subfiles not handled
	new_skynet.metadata.subfiles.emplace_back({
		subpath,
		{ {}, subpath, 0, parent->file.len }
	})

	sia::skynet::subfile new_subfile = { {}, subpath, 0 };
	std::shared_ptr<skydir> new_dir;
	std::shared_ptr<sia::skynet::response> pending_skynet;

	skydir * ancestor = parent;
	while (ancestor->parent) {
		
		auto ancestral_skynet = ancestor->skynet;
		auto new_skynet = new skydir(*ancestral_skynet);
		new_skynet->skylink.clear();

		// the topmost element gets pushed to the back.
		if (!pending_skynet) {
			new_subfile.offset = new_skynet->data.size();
			new_skynet->metadata.subfiles.emplace_back({
				new_subfile.filename,
				new_subfile
			})
		} else {
			
		}

		// but following elements will simple have a link updated.
		// we'll want to decide what this link looks like.
		// it's probably a static contenttype with data that is the sia:// link
		// so the data will be changing.  that's all.
		
		// so, when we make a new subfile, we'll want to replace with the old.
		new_subfile.contenttype = new_skynet->metadata.contenttype;
		new_subfile.filename = new_skynet->metadata.filename;
		
		while (ancestor->skynet == ancestral_skynet) {
			ancestor = ancestor->parent;
		}
	}
	
	auto skydir = parent.parent; 
	parent.parent

	// parent's skylink will change
	// we'll have to reupload parent
	// we don't want to update our tree until we've made all the subchanges.

	// there's a lot of possible needless rewriting here ..... we don't really want to update everything until the file is flushed or closed
	// updating should happen with kinda ghost entries

	// we could maybe keep a skynet::response object that doesn't actually have a skylink
}

int SiaSkynetFS::fuse_open(const char *path, struct fuse_file_info *fi)
{
	if (readonly && (fi->flags & O_ACCMODE) != O_RDONLY) {
		return -EACCES;
	}

	try {
		fi->fh = static_cast<decltype(fi->fh)>(get(path));
		return 0;
	} catch (decltype(_EACCES) error) {
		return error;
	}
}

int SiaSkynetFS::fuse_opendir(const char * path, struct fuse_file_info *)
{
	if (readonly && (fi->flags & O_ACCMODE) != O_RDONLY) {
		return -EACCES;
	}

	try {
		fi->fh = static_cast<decltype(fi->fh)>(get(path));
		return 0;
	} catch (decltype(_EACCES) error) {
		return error;
	}
}

int SiaSkynetFS::fuse_readdir(const char * path, void * buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{

	skydir * dir = static_cast<skydir*>(fi->fh);

	// TODO walk path, enumerate contents, etc
}

int SiaSkynetFS::fuse_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info * fi)
{
}

int main(int argc, char *argv[])
{
	SiaSkynetFS fs;
	return fs.run(argc, argv);
}


// DELETE BELOW HERE

// ====
// we'll want a contenttype that contains a skyjlink
// text/x.uri
// ====


// ran into a cognitive memory issue

#include "Fuse.h"
#include "Fuse-impl.h"

#include "siaskynet.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

class SiaSkynetFS : public Fusepp::Fuse<SiaSkynetFS>
{
public:
	using skynet_t = sia::skynet::response;
	using subfile_t = sia::skynet::response::subfile;

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

	class sky_entry {
	public:
		std::shared_ptr<sky_entry> const parent;

		sky_entry * skynet_entry();
		//skynet_t * skynet() { return skynet_entry()->_skynet; }

		//void get(sky_entry * & skynet_entry, subfile_t const * & subfile);

		void create(std::string filename);

		void point_to(sky_entry * skynet_entry);

		void resize(size_t length);

		void write(size_t offset, size_t length, void * data);

	private:
		subfile_t & subfile();
		void read(sky_entry * skynet_entry, size_t offset, size_t length, void * data);
		void write(sky_entry * skynet_entry, size_t offset, size_t length, void const * data);
		void resize(sky_entry * skynet_entry, subfile_t & subfile, size_t length);

		std::shared_ptr<skynet_t> _skynet; // only set if depth == 0
		size_t const _subfile; // only set if depth > 0
		// TODO: do we want a pointer to a linked subentry or anything?
			// when iterating the members of a folder, we'll want to find that

		static thread_local std::vector<size_t> subfiles;

		std::mutex entry_lock;

		struct mem_link {
			static int constexpr correct_prefix = 'mem:';
			int prefix;
			sky_entry * link;
		};
	};

	sky_entry & get(std::string const & path);

	std::mutex tree_lock;
	std::unordered_map<std::string, sky_entry> tree;
};

#include "cpr/util.h"

SiaSkynetFS::SiaSkynetFS(bool readonly, size_t rewriting_threshold, std::string initial_skylink)
: readonly(readonly), rewriting_threshold(rewriting_threshold)
{
	if (!initial_skylink.size()) {
		initial_skylink = skynet.upload("/", {});
	}

	std::shared_ptr<response> root_response(new response(skynet.download(initial_skylink)));
	tree.emplace(std::string("/"), skydir{root_response, root_response->metadata});
}

std::string SiaSkynetFS::skylink()
{
	return get("/").skynet->skylink;
}

void SiaSkynetFS::sky_entry::get(sky_entry * & skynet_entry, subfile_t const * & subfile)
{
	skynet_entry = this->skynet_entry();
	subfile = skynet_entry->subfile();
}

SiaSkynetFS::subfile_t & SiaSkynetFS::sky_entry::subfile()
{
	subfile_t * subfile = &_skynet->metadata;

	signed size_t index = subfiles.size() - 2;

	while (index >= 0) {
		subfile = &subfile->subfiles[subfiles[index]];
		-- index;
	}

	return * subfile;
}

SiaSkynetFS::skynet_t * SiaSkynetFS::sky_entry::skynet_entry()
{
	subfiles.clear();

	subfiles.push_back(_subfile);
	skydir * parent = this;
	while (!parent->_skynet) {
		parent = parent->parent;
		subfiles.push_back(parent->_subfile);
	}
	return parent;
}

struct entry_work {
	entry_work(sky_entry * skynet_entry)
	: lock(skynet_entry->entry_lock), entry(skynet_entry), mutated(false)
	{ }
	void mutating() {
		if (!entry->_skynet->skylink.size()) {
			return;
		}

		mutated = true;

		auto new_skynet = new SiaSkynetFS::skynet_t(*(entry->_skynet));
		new_skynet->skylink.clear();
		entry->_skynet = new_skynet;

		// subfile references are now invalid!
		// have to be re-retrieve with ->subfile() call
	}
	~entry_work()
	{
		if (mutated && entry->parent) {
			entry.parent->point_to(entry);
		}
	}

	std::lock_guard<std::mutex> lock;
	skynet_entry * entry;
	bool mutated
}

void SiaSkynetFS::sky_entry::create(std::string filename)
{
	sky_entry * skynet_entry = this->skynet_entry();

	entry_work work(skynet_entry);
	
	work.mutating();

	subfile_t & subfile = skynet_entry->subfile();

	subfile.metadata.subfiles.emplace_back({{}, filename, 0, subfile.metadata.len})
}

void SiaSkynetFS::sky_entry::point_to(sky_entry * sub_skynet_entry)
{
	// we've already been created as a link so our contenttype and length are correct

	std::string & skylink = sub_skynet_entry->_skynet->skylink;
	if (skylink.size() == 51) {
		write(0, skylink.size(), skylink.data());
	} else {
		sky_entry * skynet_entry = this->skynet_entry();
		mem_link link;
		read(skynet_entry, 0, sizeof(link), &link);
		if (link.link != sub_skynet_entry) {
			link.link = sub_skynet_entry;
			write(skynet_entry, 0, sizeof(link), &link);
		}
	}
}

void SiaSkynetFS::sky_entry::read(size_t offset, size_t length, void * data)
{
	sky_entry * skynet_entry = this->skynet_entry();

	read(skynet_entry, offset, length, data);
}

void SiaSkynetFS::sky_entry::write(size_t offset, size_t length, void const * data)
{
	sky_entry * skynet_entry = this->skynet_entry();

	write(skynet_entry, offset, length, data);
}

void SiaSkynetFS::sky_entry::read(sky_entry * skynet_entry, size_t offset, size_t length, void * data)
{
	entry_work work(skynet_entry);
	auto subfile = skynet_entry->subfile();
	auto reference = skynet_entry->_skynet->data.begin() + subfile.offset + offset;
	std::copy(reference, reference + length, (uint8_t*)data);
}

void shift(SiaSkynetFS::subfile_t & subfile, size_t offset, signed size_t change)
{
	if (subfile.offset + subfile.len <= offset) { return; }
	if (subfile.offset > offset) { subfile.offset += change; }
	for (auto * subsubfile : subfile.subfiles) {
		shift(*subsubfile, offset, change);
	}
}

// TODO: mutating means need-new-subfile
void SiaSkynetFS::sky_entry::resize(sky_entry * skynet_entry, subfilie_t & subfile, size_t length)
{
	if (change == 0) { return; }

	shift(skynet_entry.metadata, subfile.offset, length - subfile.len);
	subfile.len = length;
}

void SiaSkynetFS::sky_entry::write(sky_entry * skynet_entry, size_t offset, size_t length, void const * data)
{
	entry_work work(skynet_entry);
	work.mutating();

	subfile_t & subfile = skyet_entry->subfile();

	if (offset + length > subfile.len) {
		resize(offset + length);
	}

	auto reference = skynet_entry->_skynet->data.begin() + subfile.offset + offset;
	std::copy((uint8_t*)data, (uint8_t*)data + length, reference);
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

	std::shared_ptr<sia::skynet::response> new_skynet = new sia::skynet::response(*(parent->skynet));
	new_skynet->skylink.clear();

	// TODO: sub-subfiles not handled
	new_skynet->metadata.subfiles.emplace_back({
		subpath,
		{ {}, subpath, 0, parent->file.len }
	})
	std::shared_ptr<skydir> new_dir = new skydir{new_skynet, new_skynet->metadata

	std::shared_ptr<sia::skynet::response> pending_skynet;

	skydir * ancestor = parent;
	while (ancestor->parent) {
		
		while (ancestor->parent->skynet == ancestor) {
			ancestor = ancestor->parent;
		}
		
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

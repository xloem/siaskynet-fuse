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
	int fuse_opendir(const char *path, struct fuse_file_info *fi);
	int fuse_open(const char *path, struct fuse_file_info *fi);
	int fuse_releasedir(const char *path, struct fuse_file_info *fi);
	int fuse_release(const char *path, struct fuse_file_info *fi);
	int fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
	int fuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
	int fuse_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info * fi);

	static int getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
	{ return this_()->fuse_getattr(path, stbuf, fi); }
	static int opendir(const char *path, struct fuse_file_info *fi)
	{ return this_()->fuse_opendir(path, fi); }
	static int open(const char *path, struct fuse_file_info *fi)
	{ return this_()->fuse_open(path, fi); }
	static int releasedir(const char *path, struct fuse_file_info *fi)
	{ return this_()->fuse_releasedir(path, fi); }
	static int release(const char *path, struct fuse_file_info *fi)
	{ return this_()->fuse_release(path, fi); }
	static int readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
	{ return this_()->fuse_readdir(path, buf, filler, offset, fi, flags); }
	static int read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
	{ return this_()->fuse_read(path, buf, size, offset, fi); }
	static int write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info * fi)
	{ return this_()->fuse_write(path, buf, size, offset, fi); }

	void sync();
	std::string skylink();

private:
	sia::skynet skynet;

	bool readonly;
	size_t rewriting_threshold;

	class sky_entry {
	public:
		sky_entry(std::unique_ptr<skynet_t> && skynet, std::shared_ptr<sky_entry> parent = {});
		sky_entry(std::shared_ptr<sky_entry> parent, size_t subfile);
std::shared_ptr<sky_entry> const parent;

		skynet_t const & skynet() { return *skynet_entry()->_skynet; }
		size_t size() { skynet_entry(); return subfile().len; }

		//skynet_t * skynet() { return skynet_entry()->_skynet; }

		//void get(sky_entry * & skynet_entry, subfile_t const * & subfile);

		std::vector<std::string> subnames();
		size_t create(std::string filename);

		void point_to(sky_entry * skynet_entry);

		void resize(size_t length);

		void read(size_t offset, size_t length, void * data);
		void write(size_t offset, size_t length, void const * data);

	private:
		sky_entry * skynet_entry();
		subfile_t & subfile();

		void read(sky_entry * skynet_entry, size_t offset, size_t length, void * data);
		void write(sky_entry * skynet_entry, size_t offset, size_t length, void const * data);
		void resize(sky_entry * skynet_entry, subfile_t & subfile, size_t length);

		std::shared_ptr<skynet_t> _skynet; // only set if depth == 0
		size_t _subfile; // only set if depth > 0
		// TODO: do we want a pointer to a linked subentry or anything?
			// when iterating the members of a folder, we'll want to find that

		std::mutex entry_lock;

		struct mem_link {
			static int constexpr correct_prefix = 'mem:';
			int prefix;
			sky_entry * link;
		};

		struct work {
			work(sky_entry * skynet_entry);
			void mutating();
			~work();

			std::lock_guard<std::mutex> lock;
			sky_entry * entry;
			bool mutated;
		};
	};

	std::shared_ptr<sky_entry> get(std::string const & path);

	std::mutex tree_lock;
	std::unordered_map<std::string, std::shared_ptr<sky_entry>> tree;
};

#include "cpr/util.h"

SiaSkynetFS::SiaSkynetFS(bool readonly, size_t rewriting_threshold, std::string initial_skylink)
: readonly(readonly), rewriting_threshold(rewriting_threshold)
{
	if (!initial_skylink.size()) {
		initial_skylink = skynet.upload("/", {});
	}

	std::unique_ptr<skynet_t> root(new skynet_t(skynet.download(initial_skylink)));
	tree.emplace(std::string("/"), std::make_shared<sky_entry>(std::move(root)));
}

std::string SiaSkynetFS::skylink()
{
	return get("/")->skynet().skylink;
}

SiaSkynetFS::sky_entry::sky_entry(std::unique_ptr<skynet_t> && skynet, std::shared_ptr<sky_entry> parent)
: parent(parent), _skynet(skynet.release())
{ }

SiaSkynetFS::sky_entry::sky_entry(std::shared_ptr<sky_entry> parent, size_t subfile)
: parent(parent), _subfile(subfile)
{ }

/*
void SiaSkynetFS::sky_entry::get(sky_entry * & skynet_entry, subfile_t const * & subfile)
{
	skynet_entry = this->skynet_entry();
	subfile = skynet_entry->subfile();
}
*/

static thread_local std::vector<size_t> subfiles;

SiaSkynetFS::sky_entry * SiaSkynetFS::sky_entry::skynet_entry()
{
	subfiles.clear();

	sky_entry * parent = this;
	while (!parent->_skynet) {
		subfiles.push_back(parent->_subfile);
		parent = parent->parent.get();
	}
	return parent;
}

SiaSkynetFS::subfile_t & SiaSkynetFS::sky_entry::subfile()
{
	subfile_t * subfile = &_skynet->metadata;

	size_t index = subfiles.size();

	while (index > 0) {
		-- index;
		subfile = &subfile->subfiles[subfiles[index]].second;
	}

	return * subfile;
}

SiaSkynetFS::sky_entry::work::work(sky_entry * skynet_entry)
: lock(skynet_entry->entry_lock), entry(skynet_entry), mutated(false)
{ }
void SiaSkynetFS::sky_entry::work::mutating() {
	if (!entry->_skynet->skylink.size()) {
		return;
	}

	mutated = true;

	auto new_skynet = new SiaSkynetFS::skynet_t(*(entry->_skynet));
	new_skynet->skylink.clear();
	entry->_skynet.reset(new_skynet);

	// subfile references are now invalid!
	// have to be re-retrieve with ->subfile() call
}
SiaSkynetFS::sky_entry::work::~work()
{
	if (mutated && entry->parent) {
		entry->parent->point_to(entry);
	}
}

std::vector<std::string> SiaSkynetFS::sky_entry::subnames()
{
	sky_entry * skynet_entry = this->skynet_entry();
	sky_entry::work work(skynet_entry);
	subfile_t & file = skynet_entry->subfile();

	std::vector<std::string> result;
	for (auto & subfile : file.subfiles) {
		result.push_back(subfile.first);
	}
	return result;
}
size_t SiaSkynetFS::sky_entry::create(std::string filename)
{
	sky_entry * skynet_entry = this->skynet_entry();

	sky_entry::work work(skynet_entry);
	
	work.mutating();

	subfile_t & subfile = skynet_entry->subfile();

	subfile.subfiles.emplace_back(filename, subfile_t{{}, filename, 0, subfile.offset + subfile.len});

	// race condition prevented by tree_lock
	return subfile.subfiles.size() - 1;
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
	sky_entry::work work(skynet_entry);
	auto subfile = skynet_entry->subfile();
	auto reference = skynet_entry->_skynet->data.begin() + subfile.offset + offset;
	std::copy(reference, reference + length, (uint8_t*)data);
}

void shift(SiaSkynetFS::subfile_t & subfile, size_t offset, long long change)
{
	if (subfile.offset + subfile.len <= offset) { return; }
	if (subfile.offset > offset) { subfile.offset += change; }
	for (auto & entry : subfile.subfiles) {
		shift(entry.second, offset, change);
	}
	if (subfile.offset <= offset) {
		subfile.len += change;
	}
}

void SiaSkynetFS::sky_entry::resize(sky_entry * skynet_entry, subfile_t & subfile, size_t length)
{
	if (length == subfile.len) { return; }

	shift(skynet_entry->_skynet->metadata, subfile.offset, length - (long long)subfile.len);
}

void SiaSkynetFS::sky_entry::write(sky_entry * skynet_entry, size_t offset, size_t length, void const * data)
{
	sky_entry::work work(skynet_entry);
	work.mutating();

	subfile_t & subfile = skynet_entry->subfile();

	if (offset + length > subfile.len) {
		resize(skynet_entry, subfile, offset + length);
	}

	auto reference = skynet_entry->_skynet->data.begin() + subfile.offset + offset;
	std::copy((uint8_t*)data, (uint8_t*)data + length, reference);
}

#include <iostream> // DEBUG
std::shared_ptr<SiaSkynetFS::sky_entry> SiaSkynetFS::get(std::string const & path)
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

	auto parent = get(superpath);

	std::lock_guard<std::mutex> lock(tree_lock);

	auto subindex = parent->create(subpath);
	return tree.emplace(path, std::make_shared<sky_entry>(parent, subindex)).first->second;
}

int SiaSkynetFS::fuse_open(const char *path, struct fuse_file_info *fi)
{
	if (readonly && (fi->flags & O_ACCMODE) != O_RDONLY) {
		return -EACCES;
	}

	std::shared_ptr<SiaSkynetFS::sky_entry> * entry = new std::shared_ptr<SiaSkynetFS::sky_entry>(get(path));

	try {
		fi->fh = reinterpret_cast<decltype(fi->fh)>(entry);
		return 0;
	} catch (decltype(-EACCES) error) {
		return error;
	}
}

int SiaSkynetFS::fuse_opendir(const char * path, struct fuse_file_info *fi)
{
	if (readonly && (fi->flags & O_ACCMODE) != O_RDONLY) {
		return -EACCES;
	}

	std::shared_ptr<SiaSkynetFS::sky_entry> * entry = new std::shared_ptr<SiaSkynetFS::sky_entry>(get(path));

	try {
		fi->fh = reinterpret_cast<decltype(fi->fh)>(entry);
		return 0;
	} catch (decltype(-EACCES) error) {
		return error;
	}
}

int SiaSkynetFS::fuse_release(const char *path, struct fuse_file_info *fi)
{
	auto * entry = reinterpret_cast<std::shared_ptr<SiaSkynetFS::sky_entry>*>(fi->fh);
	delete entry;
	return 0;
}

int SiaSkynetFS::fuse_releasedir(const char * path, struct fuse_file_info *fi)
{
	auto * entry = reinterpret_cast<std::shared_ptr<SiaSkynetFS::sky_entry>*>(fi->fh);
	delete entry;
	return 0;
}

int SiaSkynetFS::fuse_readdir(const char * path, void * buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	auto * dir = reinterpret_cast<std::shared_ptr<SiaSkynetFS::sky_entry>*>(fi->fh);

	filler(buf, ".", NULL, 0, FUSE_FILL_DIR_PLUS);
	filler(buf, "..", NULL, 0, FUSE_FILL_DIR_PLUS);
	
	for (auto & subfile : (*dir)->subnames()) {
		filler(buf, subfile.c_str(), NULL, 0, FUSE_FILL_DIR_PLUS);
	}

	return 0;
}

int SiaSkynetFS::fuse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	memset(stbuf, 0, sizeof(struct stat));
	// we don't really have a way of distinguishing between a dir and a file
	
	auto * entry = reinterpret_cast<std::shared_ptr<SiaSkynetFS::sky_entry>*>(fi->fh);
	
	stbuf->st_mode = S_IFDIR | 0755;
	stbuf->st_nlink = entry->use_count();
	stbuf->st_size = (*entry)->size();

	return 0;
}

int SiaSkynetFS::fuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info * fi)
{
	auto * dir = reinterpret_cast<std::shared_ptr<sky_entry>*>(fi->fh);
	(*dir)->read(offset, size, buf);
	return size;
}

int SiaSkynetFS::fuse_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info * fi)
{
	auto * dir = reinterpret_cast<std::shared_ptr<sky_entry>*>(fi->fh);
	(*dir)->write(offset, size, buf);
	return size;
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

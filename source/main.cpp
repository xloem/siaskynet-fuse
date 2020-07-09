#include "Fuse.h"

#include "siaskynet.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

#include <iostream> // DEBUG

class SiaSkynetFS : public Fusepp::Fuse<SiaSkynetFS>
{
public:
	using skynet_t = sia::skynet::response;
	using subfile_t = sia::skynet::response::subfile;

	SiaSkynetFS(bool readonly = false, size_t rewriting_threshold = 4096, std::string initial_skylink = {});

	int getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
	int mkdir(const char *path, mode_t mode);
	int mknod(const char *path, mode_t mode, dev_t dev);
	int opendir(const char *path, struct fuse_file_info *fi);
	int unlink(const char *path);
	int open(const char *path, struct fuse_file_info *fi);
	int releasedir(const char *path, struct fuse_file_info *fi);
	int release(const char *path, struct fuse_file_info *fi);
	int readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
	int read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
	int write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info * fi);
	int chmod(const char *path, mode_t mode, struct fuse_file_info * fi);

	std::string skylink();

private:
	sia::skynet skynet;

	bool readonly;
	size_t rewriting_threshold;

	class sky_entry {
	public:
		sky_entry(std::unique_ptr<skynet_t> && skynet, size_t subfile = 0, std::shared_ptr<sky_entry> parent = {});
		sky_entry(std::shared_ptr<sky_entry> parent, size_t subfile);

		std::shared_ptr<sky_entry> const parent;

		skynet_t const & skynet() { return *skynet_entry()->_skynet; }
		size_t size() { return skynet_entry()->subfile().len; }

		//skynet_t * skynet() { return skynet_entry()->_skynet; }

		//void get(sky_entry * & skynet_entry, subfile_t const * & subfile);

		std::vector<std::string> subnames();
		size_t find(std::string filename);
		size_t create(std::string filename, uint32_t mode);
		subfile_t & get(size_t subfile);

		void point_to(sky_entry * skynet_entry);

		void resize(size_t length);
		void unlink();

		void read(size_t offset, size_t length, void * data);
		void write(size_t offset, size_t length, void const * data);

		void chmod(uint32_t mode);
		uint32_t mode() { return _mode; }

		void sync(sia::skynet & portal);

		bool is_a_root() { return (bool)_skynet; }
		std::string skylink() { return is_a_root() ? _skynet->skylink : ""; }

	private:
		sky_entry * skynet_entry();
		subfile_t & subfile();

		std::string contenttype();
		void contenttype(std::string contenttype);

		void read(sky_entry * skynet_entry, size_t offset, size_t length, void * data);
		void write(sky_entry * skynet_entry, size_t offset, size_t length, void const * data);
		void resize(sky_entry * skynet_entry, subfile_t & subfile, size_t length);

		void sync(sia::skynet & portaL, sky_entry * skynet_entry);
		static void sync_check(sia::skynet & portal, sky_entry * skynet_entry, subfile_t & subfile, std::vector<sia::skynet::upload_data> & data, std::string path);

		std::shared_ptr<skynet_t> _skynet; // only set if depth == 0
		size_t _subfile; // only set if depth > 0
		uint32_t _mode;

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

	std::shared_ptr<sky_entry> & get(std::string path, bool create = false, uint32_t mode = 0);

	std::mutex tree_lock;
	std::unordered_map<std::string, std::shared_ptr<sky_entry>> tree;
};

#include "cpr/util.h"

SiaSkynetFS::SiaSkynetFS(bool readonly, size_t rewriting_threshold, std::string initial_skylink)
: readonly(readonly), rewriting_threshold(rewriting_threshold)
{
	std::cerr << "BOOT" << std::endl;
	std::unique_ptr<skynet_t> root;
	std::cerr << "ROOT" << std::endl;
	if (initial_skylink.size()) {
		root.reset(new skynet_t(skynet.download(initial_skylink)));
	} else {
		root.reset(new skynet_t{{}, sia::skynet::default_options});
	}

	std::cerr << "SETUP" << std::endl;
	tree.emplace(std::string("/"), std::make_shared<sky_entry>(std::move(root)));
	std::cerr << "WHOO" << std::endl;
}

std::string SiaSkynetFS::skylink()
{
	auto root = get("/");
	root->sync(skynet);
	return root->skynet().skylink;
}

SiaSkynetFS::sky_entry::sky_entry(std::unique_ptr<skynet_t> && skynet, size_t subfile, std::shared_ptr<sky_entry> parent)
: parent(parent), _skynet(skynet.release()), _subfile(subfile)
{
	std::string contenttype;
	if (parent) {
		contenttype = parent->get(subfile).contenttype;
	} else {
		contenttype = _skynet->metadata.contenttype;
	}
	this->contenttype(contenttype);
}

SiaSkynetFS::sky_entry::sky_entry(std::shared_ptr<sky_entry> parent, size_t subfile)
: parent(parent), _subfile(subfile)
{
	this->contenttype(parent->get(subfile).contenttype);
}

void SiaSkynetFS::sky_entry::contenttype(std::string content_type)
{
	auto parts = cpr::util::split(content_type, ';');
	for (auto & part : parts) {
		if (part.compare(0, 5, "mode=") == 0) {
			sscanf(part.c_str() + 5, "%o", &_mode);
			break;
		}
	}
	if (!parts.size()) {
		_mode |= S_IFDIR;
	} else if (parts[0] == "multipart/x.directory") {
		_mode |= S_IFDIR;
	}
}

std::string SiaSkynetFS::sky_entry::contenttype()
{
	std::string contenttype;
	if (S_ISDIR(_mode)) {
		contenttype = "multipart/x.directory";
	} else {
		contenttype = "application/octet-stream";
	}

	char mode_characters[64];
	snprintf(mode_characters, sizeof(mode_characters), "%o", _mode);
	contenttype = contenttype + ";mode=" + mode_characters;

	return contenttype;
}

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
size_t SiaSkynetFS::sky_entry::find(std::string filename)
{
	std::cerr << "SUB GET" << std::endl;
	sky_entry * skynet_entry = this->skynet_entry();
	std::cerr << "from: " << skynet_entry->_skynet->metadata.filename << std::endl;

	sky_entry::work work(skynet_entry);
	
	std::cerr << "set up exclusive work" << std::endl;

	subfile_t & subfile = skynet_entry->subfile();

	std::cerr << "subfile: " << subfile.filename << std::endl;

	size_t i = 0;
	for (auto & subsubfile : subfile.subfiles) {
		if (subsubfile.second.filename == filename) {
			return i;
		}
		++i;
	}

	throw -ENOENT;
}
size_t SiaSkynetFS::sky_entry::create(std::string filename, uint32_t mode)
{
	std::cerr << "SUB CREATE" << std::endl;
	sky_entry * skynet_entry = this->skynet_entry();
	std::cerr << "from: " << skynet_entry->_skynet->metadata.filename << std::endl;

	sky_entry::work work(skynet_entry);
	
	work.mutating();
	std::cerr << "set up exclusive work" << std::endl;

	subfile_t & subfile = skynet_entry->subfile();

	std::cerr << "subfile: " << subfile.filename << std::endl;

	subfile.subfiles.emplace_back(filename, subfile_t{{}, filename, 0, subfile.offset + subfile.len});

	// race condition prevented by tree_lock
	return subfile.subfiles.size() - 1;
}

SiaSkynetFS::subfile_t & SiaSkynetFS::sky_entry::get(size_t subfile)
{
	sky_entry * skynet_entry = this->skynet_entry();
	sky_entry::work work(skynet_entry);
	subfile_t & file = skynet_entry->subfile();
	return file.subfiles[subfile].second;
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

void SiaSkynetFS::sky_entry::sync(sia::skynet & portal)
{
	sky_entry * skynet_entry = this->skynet_entry();
	sync(portal, skynet_entry);
}

void SiaSkynetFS::sky_entry::sync_check(sia::skynet & portal, sky_entry * skynet_entry, subfile_t & subfile, std::vector<sia::skynet::upload_data> & data, std::string path)
{
	auto & skynet = *skynet_entry->_skynet;

	if (subfile.contenttype.compare(0, 12, "text/url-list") == 0 && sizeof(mem_link) <= subfile.len) {
		mem_link link;
		std::copy(skynet.data.begin() + subfile.offset, skynet.data.begin() + subfile.offset + sizeof(link), (uint8_t*)&link);
		if (link.prefix == mem_link::correct_prefix) {
			link.link->sync(portal, link.link);
			std::copy(link.link->_skynet->skylink.begin(), link.link->_skynet->skylink.end(), skynet.data.begin() + subfile.offset);
		}
	}

	if (subfile.subfiles.size()) {
		if (&skynet.metadata != &subfile) {
			path += subfile.filename + "/";
		}
		for (auto & entry : subfile.subfiles) {
			sync_check(portal, skynet_entry, entry.second, data, path);
		}
	} else {
		data.emplace_back(path + subfile.filename, std::vector<uint8_t>(skynet.data.begin() + subfile.offset, skynet.data.begin() + subfile.offset + subfile.len), subfile.contenttype);
	}
}

void SiaSkynetFS::sky_entry::sync(sia::skynet & portal, sky_entry * skynet_entry)
{
	skynet_t & skynet = *skynet_entry->_skynet;
	std::vector<sia::skynet::upload_data> data;

	sync_check(portal, skynet_entry, skynet.metadata, data, "");

	skynet.skylink = portal.upload(skynet.metadata.filename, data);
}

void SiaSkynetFS::sky_entry::read(size_t offset, size_t length, void * data)
{
	sky_entry * skynet_entry = this->skynet_entry();

	read(skynet_entry, offset, length, data);
}

void SiaSkynetFS::sky_entry::write(size_t offset, size_t length, void const * data)
{
	sky_entry * skynet_entry = this->skynet_entry();

	std::cerr << "WRITE WRAPPER" << std::endl;

	write(skynet_entry, offset, length, data);
}

void SiaSkynetFS::sky_entry::resize(size_t length)
{
	sky_entry * skynet_entry = this->skynet_entry();

	sky_entry::work work(skynet_entry);
	work.mutating();

	resize(skynet_entry, skynet_entry->subfile(), length);
}

void SiaSkynetFS::sky_entry::unlink()
{
	sky_entry * skynet_entry = this->skynet_entry();

	sky_entry::work work(skynet_entry);
	work.mutating();

	auto & subfile = skynet_entry->subfile();

	// TODO: remove children
	resize(skynet_entry, subfile, 0);

	// TODO: remove following siblings

	// note: we do not have access to children from a parent.
	// i think it would be meaningful to get rid of the unordered_tree structure
	// and use sky_entries for everything.
}

void SiaSkynetFS::sky_entry::read(sky_entry * skynet_entry, size_t offset, size_t length, void * data)
{
	sky_entry::work work(skynet_entry);
	auto subfile = skynet_entry->subfile();
	std::cerr << "READ FROM " << subfile.filename << std::endl;
	std::cerr << "READ IN " << skynet_entry->_skynet->metadata.filename << " AT " << subfile.offset + offset << " OF " << length << std::endl;
	std::cerr << "DATA LENGTH = " << skynet_entry->_skynet->data.size() << std::endl;
	auto reference = skynet_entry->_skynet->data.begin() + subfile.offset + offset;
	std::cerr << "MOVING DATA" << std::endl;
	std::copy(reference, reference + length, (uint8_t*)data);
	std::cerr << "READ FINISH" << std::endl;
}

void shift(SiaSkynetFS::subfile_t & subfile, size_t offset, long long change)
{
	// have not fully reviewed this recursive function for correctness
	if (subfile.offset + subfile.len < offset) { return; }
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

	std::cerr << "RESIZE " << subfile.len << " TO " << length << std::endl;

	long long change = length - (long long)subfile.len;

	shift(skynet_entry->_skynet->metadata, subfile.offset, change);
	skynet_entry->_skynet->data.resize(skynet_entry->_skynet->data.size() + change);

	std::cerr << "RESIZE DONE" << std::endl;
}

void SiaSkynetFS::sky_entry::write(sky_entry * skynet_entry, size_t offset, size_t length, void const * data)
{
	sky_entry::work work(skynet_entry);
	work.mutating();

	subfile_t & subfile = skynet_entry->subfile();

	if (offset + length > subfile.len) {
		resize(skynet_entry, subfile, offset + length);
	}

	std::cerr << "FIND DATA" << std::endl;

	auto reference = skynet_entry->_skynet->data.begin() + subfile.offset + offset;
	std::cerr << "MOVE DATA TO " << (subfile.offset + offset) << " IN BUFFER SIZED " << skynet_entry->_skynet->data.size() << std::endl;
	std::copy((uint8_t*)data, (uint8_t*)data + length, reference);
	std::cerr << "FINISH" << std::endl;
}

void SiaSkynetFS::sky_entry::chmod(uint32_t mode)
{
	sky_entry * skynet_entry = this->skynet_entry();
	sky_entry::work work(skynet_entry);

	work.mutating();

	subfile_t & subfile = skynet_entry->subfile();

	_mode = mode;
	subfile.contenttype = this->contenttype();
}

std::shared_ptr<SiaSkynetFS::sky_entry> & SiaSkynetFS::get(std::string path, bool create, uint32_t mode)
{
	if (!create) {
		auto result = tree.find(path);
		if (result != tree.end()) {
			return result->second;
		}
	} else {
		if (readonly) {
			throw -EACCES;
		}
		if (tree.find(path) != tree.end()) {
			throw -EEXIST;
		}
	}

	std::cerr << "NEW PATH FOR CACHE: " << path << " (create = " << create << ")" << std::endl;

	// not readonly, and path doesn't exist.
	auto path_components = cpr::util::split(path, '/');
	auto subpath = path_components.back();
	auto superpath = path;
	
	superpath.resize(superpath.size() - subpath.size());
	if (superpath.size() > 1) { superpath.resize(superpath.size() - 1); }

	std::cerr << "PARENT: " << superpath << std::endl;

	auto parent = get(superpath);
	if (!parent) { throw -ENOENT; }

	std::cerr << "GOT PARENT: " << superpath << std::endl;

	std::lock_guard<std::mutex> lock(tree_lock);

	// when creating a file we know the mode in advance.
	// let's pass that to create so as to unify how it is handled.

	size_t subindex;
	if (create) {
		subindex = parent->create(subpath, mode);
	} else {
		subindex = parent->find(subpath);
	}
	std::cerr << "ADDING !" << std::endl;
	auto emplacement = tree.emplace(path, std::make_shared<sky_entry>(parent, subindex));
	return emplacement.first->second;
}

int SiaSkynetFS::mkdir(const char *path, mode_t mode)
{
	try {
		mode |= S_IFDIR;
		auto & entry = get(path, true, mode);

		return 0;
	} catch (decltype(-EACCES) error) {
		return error;
	}

}

int SiaSkynetFS::mknod(const char *path, mode_t mode, dev_t dev)
{
	try {
		auto & entry = get(path, true, mode);
		entry->chmod(mode);

		return 0;
	} catch (decltype(-EACCES) error) {
		return error;
	}
}

int SiaSkynetFS::unlink(const char *path)
{
	try {
		auto & entry = get(path);
		entry->unlink();

		return 0;
	} catch (decltype(-EACCES) error) {
		return error;
	}
}

int SiaSkynetFS::open(const char *path, struct fuse_file_info *fi)
{
	std::cerr << "open " << path << std::endl;

	if (readonly && (fi->flags & O_ACCMODE) != O_RDONLY) {
		return -EACCES;
	}

	try {
		std::shared_ptr<SiaSkynetFS::sky_entry> * entry = new std::shared_ptr<SiaSkynetFS::sky_entry>(get(path, fi->flags & O_CREAT));

		if ((fi->flags & O_ACCMODE) != O_RDONLY && (fi->flags & O_TRUNC)) {
			(*entry)->resize(0);
		}

		fi->fh = reinterpret_cast<decltype(fi->fh)>(entry);
		return 0;
	} catch (decltype(-EACCES) error) {
		return error;
	}
}

int SiaSkynetFS::opendir(const char * path, struct fuse_file_info *fi)
{
	std::cerr << "opendir " << path << std::endl;
	if (readonly && (fi->flags & O_ACCMODE) != O_RDONLY) {
		return -EACCES;
	}

	try {
		std::shared_ptr<SiaSkynetFS::sky_entry> * entry = new std::shared_ptr<SiaSkynetFS::sky_entry>(get(path, fi->flags & O_CREAT));
		fi->fh = reinterpret_cast<decltype(fi->fh)>(entry);
		return 0;
	} catch (decltype(-EACCES) error) {
		return error;
	}
}

int SiaSkynetFS::release(const char *path, struct fuse_file_info *fi)
{
	auto * entry = reinterpret_cast<std::shared_ptr<SiaSkynetFS::sky_entry>*>(fi->fh);
	delete entry;
	
	std::cerr << "skylink: " << skylink() << std::endl;
	std::cerr << "that was skylink" << std::endl;

	return 0;
}

int SiaSkynetFS::releasedir(const char * path, struct fuse_file_info *fi)
{
	auto * entry = reinterpret_cast<std::shared_ptr<SiaSkynetFS::sky_entry>*>(fi->fh);
	delete entry;
	return 0;
}

int SiaSkynetFS::readdir(const char * path, void * buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	auto & dir = *reinterpret_cast<std::shared_ptr<SiaSkynetFS::sky_entry>*>(fi->fh);

	filler(buf, ".", NULL, 0, FUSE_FILL_DIR_PLUS);
	filler(buf, "..", NULL, 0, FUSE_FILL_DIR_PLUS);
	
	for (auto & subfile : dir->subnames()) {
		filler(buf, subfile.c_str(), NULL, 0, FUSE_FILL_DIR_PLUS);
	}

	return 0;
}

int SiaSkynetFS::getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	memset(stbuf, 0, sizeof(struct stat));
	// we don't really have a way of distinguishing between a dir and a file
	
	try {
		auto & entry = fi ? *reinterpret_cast<std::shared_ptr<SiaSkynetFS::sky_entry>*>(fi->fh) : get(path, false);
	
		stbuf->st_nlink = entry.use_count();
		std::cerr << "SET USE COUNT = " << stbuf->st_nlink << std::endl;
		stbuf->st_mode = entry->mode();
		std::cerr << "SET MODE = " << stbuf->st_mode << std::endl;
		stbuf->st_size = entry->size();
		std::cerr << "SET SIZE = " << stbuf->st_size << std::endl;

		return 0;
	} catch (decltype(-EACCES) error) {
		return error;
	}

}

int SiaSkynetFS::read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info * fi)
{
	auto * dir = reinterpret_cast<std::shared_ptr<sky_entry>*>(fi->fh);
	(*dir)->read(offset, size, buf);
	return size;
}

int SiaSkynetFS::write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info * fi)
{
	auto * dir = reinterpret_cast<std::shared_ptr<sky_entry>*>(fi->fh);
	(*dir)->write(offset, size, buf);
	return size;
}

int SiaSkynetFS::chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	try {
		auto & entry = fi ? *reinterpret_cast<std::shared_ptr<SiaSkynetFS::sky_entry>*>(fi->fh) : get(path, false);
	
		entry->chmod(mode);

		return 0;
	} catch (decltype(-EACCES) error) {
		return error;
	}
}

int main(int argc, char *argv[])
{
	SiaSkynetFS fs;
	return fs.run(argc, argv);
}

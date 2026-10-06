#include "save-logic.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace osp {

static fs::path to_path(const std::string &utf8)
{
	return fs::u8path(utf8);
}

static std::string to_utf8(const fs::path &p)
{
	auto s = p.u8string();
	return std::string(s.begin(), s.end());
}

static std::string upper_ascii(std::string s)
{
	for (auto &c : s)
		c = (char)std::toupper((unsigned char)c);
	return s;
}

static bool is_reserved_name(const std::string &name)
{
	// Reserved Windows device names, with or without an extension.
	std::string base = upper_ascii(name.substr(0, name.find('.')));
	while (!base.empty() && base.back() == ' ')
		base.pop_back();
	if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL")
		return true;
	if (base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) && base[3] >= '1' &&
	    base[3] <= '9')
		return true;
	return false;
}

std::string sanitize_name(const std::string &input)
{
	std::string out;
	out.reserve(input.size());
	for (unsigned char c : input) {
		if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
		    c == '?' || c == '*')
			out.push_back('_');
		else
			out.push_back((char)c);
	}

	// Trim leading spaces and trailing spaces/dots (Windows silently drops trailing ones).
	size_t start = 0;
	while (start < out.size() && out[start] == ' ')
		start++;
	size_t end = out.size();
	while (end > start && (out[end - 1] == ' ' || out[end - 1] == '.'))
		end--;
	out = out.substr(start, end - start);

	if (out.empty())
		return "";

	// Cap the length (bytes), without cutting a UTF-8 character in half.
	const size_t max_len = 150;
	if (out.size() > max_len) {
		size_t cut = max_len;
		while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
			cut--;
		out = out.substr(0, cut);
		while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
			out.pop_back();
		if (out.empty())
			return "";
	}

	if (is_reserved_name(out))
		out = "_" + out;

	return out;
}

std::string strip_matching_ext(const std::string &name, const std::string &ext)
{
	if (ext.empty() || name.size() < ext.size())
		return name;
	std::string tail = name.substr(name.size() - ext.size());
	if (upper_ascii(tail) == upper_ascii(ext))
		return name.substr(0, name.size() - ext.size());
	return name;
}

std::string folder_of(const std::string &path)
{
	return to_utf8(to_path(path).parent_path());
}

bool same_path(const std::string &a, const std::string &b)
{
	std::error_code ec;
	fs::path pa = fs::weakly_canonical(to_path(a), ec);
	if (ec)
		pa = to_path(a).lexically_normal();
	fs::path pb = fs::weakly_canonical(to_path(b), ec);
	if (ec)
		pb = to_path(b).lexically_normal();
	std::string sa = to_utf8(pa), sb = to_utf8(pb);
#ifdef _WIN32
	std::transform(sa.begin(), sa.end(), sa.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	std::transform(sb.begin(), sb.end(), sb.begin(), [](unsigned char c) { return (char)std::tolower(c); });
#endif
	while (sa.size() > 1 && (sa.back() == '/' || sa.back() == '\\'))
		sa.pop_back();
	while (sb.size() > 1 && (sb.back() == '/' || sb.back() == '\\'))
		sb.pop_back();
	return sa == sb;
}

std::string build_target_path(const std::string &folder, const std::string &typed_name,
			      const std::string &original_path)
{
	fs::path orig = to_path(original_path);
	std::string ext = to_utf8(orig.extension());
	std::string stem = to_utf8(orig.stem());

	std::string name = sanitize_name(strip_matching_ext(typed_name, ext));
	if (name.empty())
		name = stem;

	fs::path dir = folder.empty() ? orig.parent_path() : to_path(folder);

	fs::path candidate = dir / to_path(name + ext);
	// If the "new" path is the original file itself, it is not a collision.
	std::error_code ec;
	int n = 2;
	while (fs::exists(candidate, ec) && !same_path(to_utf8(candidate), original_path)) {
		candidate = dir / to_path(name + " (" + std::to_string(n) + ")" + ext);
		n++;
		if (n > 9999)
			break;
	}
	return to_utf8(candidate);
}

bool move_file(const std::string &from, const std::string &to, std::string &error)
{
	std::error_code ec;
	fs::path src = to_path(from), dst = to_path(to);

	if (!fs::exists(src, ec)) {
		error = "Source file does not exist: " + from;
		return false;
	}
	if (same_path(from, to))
		return true; // already in place
	if (fs::exists(dst, ec)) {
		error = "A file already exists at the destination: " + to;
		return false;
	}

	fs::create_directories(dst.parent_path(), ec);
	if (ec) {
		error = "Could not create folder '" + to_utf8(dst.parent_path()) + "': " + ec.message();
		return false;
	}

	// Fast path: same drive.
	fs::rename(src, dst, ec);
	if (!ec)
		return true;

	// Different drive (or file briefly locked): copy, verify size, then delete the original.
	std::error_code ec2;
	auto size_before = fs::file_size(src, ec2);
	if (ec2) {
		error = "Could not read the recording: " + ec2.message();
		return false;
	}
	fs::copy_file(src, dst, fs::copy_options::none, ec2);
	if (ec2) {
		fs::remove(dst, ec2);
		error = "Could not copy the recording: " + ec2.message();
		return false;
	}
	auto size_after = fs::file_size(dst, ec2);
	if (ec2 || size_after != size_before) {
		fs::remove(dst, ec2);
		error = "Copy verification failed; the original file was left untouched.";
		return false;
	}
	fs::remove(src, ec2);
	if (ec2) {
		// Copy succeeded but the original could not be removed: report, keep both.
		error = "File copied, but the original could not be deleted: " + ec2.message();
		return false;
	}
	return true;
}

} // namespace osp

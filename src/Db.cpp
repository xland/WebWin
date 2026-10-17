#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
// JsonArray / JsonObject 迭代、 Append、 HasKey 这些模板的定义体在这儿，
// 少了它只会得到一堆 C3779（"返回 auto 的函数未定义"）
#include <winrt/Windows.Foundation.Collections.h>

#include "Db.h"

#include "App.h"
#include "Json.h"
#include "Util.h"

#include "sqlite/sqlite3.h"

namespace
{
	std::map<int, std::unique_ptr<Db>> conns;
	int lastId = 0;

	/** BLOB 在 JSON 里没有对应类型，编成 base64 字符串 */
	std::string toBase64(const void* data, size_t len)
	{
		static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		const auto* p = static_cast<const unsigned char*>(data);
		std::string out;
		out.reserve((len + 2) / 3 * 4);
		for (size_t i = 0; i < len; i += 3)
		{
			unsigned v = static_cast<unsigned>(p[i]) << 16;
			if (i + 1 < len) v |= static_cast<unsigned>(p[i + 1]) << 8;
			if (i + 2 < len) v |= static_cast<unsigned>(p[i + 2]);
			auto n = std::min<size_t>(3, len - i);
			out += table[(v >> 18) & 63];
			out += table[(v >> 12) & 63];
			out += n > 1 ? table[(v >> 6) & 63] : '=';
			out += n > 2 ? table[v & 63] : '=';
		}
		return out;
	}

	/**
	 * 拒绝 ATTACH。
	 *
	 * open 只让开发者开一个库文件。若允许 ATTACH，页面就能用任意 SQL 把磁盘上
	 * 别的库挂进来，open 那边的路径约定就形同虚设 —— 这是本域最容易被忽略的口子
	 */
	int authorize(void*, int code, const char*, const char*, const char*, const char*)
	{
		return code == SQLITE_ATTACH ? SQLITE_DENY : SQLITE_OK;
	}

	bool bindOne(sqlite3_stmt* stmt, int idx, const IJsonValue& v, std::wstring& err)
	{
		switch (v.ValueType())
		{
		case JsonValueType::Null:
			sqlite3_bind_null(stmt, idx);
			return true;
		case JsonValueType::Boolean:
			sqlite3_bind_int(stmt, idx, v.GetBoolean() ? 1 : 0);
			return true;
		case JsonValueType::Number:
		{
			auto d = v.GetNumber();
			// 整数值按 INTEGER 绑：不然 1 会被存成 1.0，比较和显示都不对
			if (d >= -9.0e18 && d <= 9.0e18 && static_cast<sqlite3_int64>(d) == d)
				sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(d));
			else sqlite3_bind_double(stmt, idx, d);
			return true;
		}
		case JsonValueType::String:
		{
			auto s = Util::wideToUtf8(v.GetString().c_str());
			sqlite3_bind_text(stmt, idx, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
			return true;
		}
		default:
			err = L"参数类型不支持：只接受 null / boolean / number / string";
			return false;
		}
	}

	/**
	 * 绑定参数。两种都收：
	 * - 数组  → 按位置绑 `?`（第 1 个元素对应第 1 个问号）
	 * - 对象  → 按名字绑，SQL 里写 `:name` / `@name` / `$name` 都能对上
	 */
	bool bind(sqlite3_stmt* stmt, const JsonObject& params, std::wstring& err)
	{
		if (!params || !params.HasKey(L"params")) return true;
		auto v = params.GetNamedValue(L"params");

		if (v.ValueType() == JsonValueType::Array)
		{
			auto arr = v.GetArray();
			for (uint32_t i = 0; i < arr.Size(); ++i)
				if (!bindOne(stmt, static_cast<int>(i + 1), arr.GetAt(i), err)) return false;
			return true;
		}

		if (v.ValueType() == JsonValueType::Object)
		{
			auto obj = v.GetObject();
			for (auto&& pair : obj)
			{
				auto name = Util::wideToUtf8(pair.Key().c_str());
				int idx = 0;
				// 三种前缀都试一遍：调用方不必关心 SQL 里用的是哪一种
				for (auto prefix : { ":", "@", "$" })
				{
					idx = sqlite3_bind_parameter_index(stmt, (prefix + name).c_str());
					if (idx) break;
				}
				if (!idx)
				{
					err = L"SQL 里没有这个参数：" + std::wstring(pair.Key().c_str());
					return false;
				}
				if (!bindOne(stmt, idx, pair.Value(), err)) return false;
			}
			return true;
		}

		err = L"参数只支持数组（? 占位）或对象（具名占位）";
		return false;
	}

	bool prepare(sqlite3* db, const std::wstring& sql, const JsonObject& params, sqlite3_stmt*& stmt, std::wstring& err)
	{
		if (sqlite3_prepare_v2(db, Util::wideToUtf8(sql).c_str(), -1, &stmt, nullptr) != SQLITE_OK)
		{
			err = L"SQL 解析失败：" + Util::utf8ToWide(sqlite3_errmsg(db));
			return false;
		}
		if (!bind(stmt, params, err))
		{
			sqlite3_finalize(stmt);
			stmt = nullptr;
			return false;
		}
		return true;
	}

	JsonObject readRow(sqlite3_stmt* stmt)
	{
		JsonObject row;
		auto cols = sqlite3_column_count(stmt);
		for (int i = 0; i < cols; ++i)
		{
			auto name = Util::utf8ToWide(sqlite3_column_name(stmt, i));
			switch (sqlite3_column_type(stmt, i))
			{
			case SQLITE_NULL:
				row.SetNamedValue(name.c_str(), JsonValue::CreateNullValue());
				break;
			case SQLITE_INTEGER:
				// int64 → double：超过 2^53 会丢精度（JS number 本来也只有这个范围）
				row.SetNamedValue(name.c_str(), JsonValue::CreateNumberValue(static_cast<double>(sqlite3_column_int64(stmt, i))));
				break;
			case SQLITE_FLOAT:
				row.SetNamedValue(name.c_str(), JsonValue::CreateNumberValue(sqlite3_column_double(stmt, i)));
				break;
			case SQLITE_TEXT:
				row.SetNamedValue(name.c_str(), JsonValue::CreateStringValue(Util::utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, i)))));
				break;
			case SQLITE_BLOB:
				row.SetNamedValue(name.c_str(), JsonValue::CreateStringValue(Util::utf8ToWide(toBase64(sqlite3_column_blob(stmt, i), sqlite3_column_bytes(stmt, i)))));
				break;
			}
		}
		return row;
	}
}

Db::Db(Page* page, ICoreWebView2Frame2* frame) : page(page), frame(frame)
{
}

Db::~Db()
{
	// close_v2：万一还有没 finalize 的语句，它也不会漏，只是把连接标成待关
	if (db) sqlite3_close_v2(db);
}

std::wstring Db::resolvePath(const std::wstring& name)
{
	// 带分隔符 / 盘符 → 开发者给了完整路径，按原样用
	if (name.find_first_of(L"/\\:") != std::wstring::npos) return name;

	auto dir = App::getDataPath() / L"db";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec); // 目录可能已存在，忽略失败
	return (dir / (name.empty() ? L"webwin.db" : name)).wstring();
}

void Db::harden(sqlite3* db)
{
	// 防御模式：禁止直接写 sqlite_master 这类系统表、禁止非常规地改视图 / 触发器
	sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
	// 禁用 ATTACH：见 authorize 的说明
	sqlite3_set_authorizer(db, authorize, nullptr);
	// 多窗口开同一个库时，别一撞锁就报 BUSY
	sqlite3_busy_timeout(db, 5000);
	// WAL：读写不互相阻塞。失败不影响使用（比如库在网络盘上），不检查返回值
	sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
}

bool Db::open(const std::wstring& name, std::wstring& err)
{
	auto path = resolvePath(name);
	// sqlite3_open 只吃 UTF-8 路径（Windows VFS 内部再转 UTF-16）
	if (sqlite3_open(Util::wideToUtf8(path).c_str(), &db) != SQLITE_OK)
	{
		// 失败时 db 也已分配，错误消息得在关掉之前取
		err = L"打开数据库失败：" + path + L"（" + Util::utf8ToWide(sqlite3_errmsg(db)) + L"）";
		sqlite3_close_v2(db);
		db = nullptr;
		return false;
	}
	harden(db);
	return true;
}

void Db::handleApi(const std::wstring& method, const JsonObject& params, Page* page, JsonObject& result, ICoreWebView2Frame2* frame)
{
	if (method == L"open")
	{
		auto conn = std::unique_ptr<Db>(new Db(page, frame));
		std::wstring err;
		if (!conn->open(Json::getString(params, L"name", L""), err))
		{
			Json::setError(result, err);
			return;
		}

		auto id = ++lastId;
		conn->id = id;
		conns.emplace(id, std::move(conn));
		Json::setOk(result);
		result.SetNamedValue(L"data", JsonValue::CreateNumberValue(id));
		return;
	}

	auto id = Json::getInt(params, L"id", -1);
	auto it = conns.find(id);
	if (it == conns.end())
	{
		Json::setError(result, L"db not found: " + std::to_wstring(id));
		return;
	}
	auto db = it->second->db;

	if (method == L"close")
	{
		conns.erase(it); // 析构里 close_v2：忘了调就一直占着文件锁
		Json::setOk(result);
		return;
	}

	auto sql = Json::getString(params, L"sql", L"");
	if (sql.empty())
	{
		Json::setError(result, L"sql missing");
		return;
	}

	if (method == L"exec")
	{
		// 建表 / 初始化这类多语句场景，不收参数（sqlite3_exec 不支持绑定）
		char* msg = nullptr;
		auto rc = sqlite3_exec(db, Util::wideToUtf8(sql).c_str(), nullptr, nullptr, &msg);
		if (rc != SQLITE_OK)
		{
			Json::setError(result, L"执行失败：" + Util::utf8ToWide(msg ? msg : sqlite3_errmsg(db)));
			sqlite3_free(msg);
			return;
		}
		Json::setOk(result);
		return;
	}

	if (method == L"run")
	{
		sqlite3_stmt* stmt = nullptr;
		std::wstring err;
		if (!prepare(db, sql, params, stmt, err))
		{
			Json::setError(result, err);
			return;
		}
		auto rc = sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE && rc != SQLITE_ROW)
		{
			Json::setError(result, L"执行失败：" + Util::utf8ToWide(sqlite3_errmsg(db)));
			return;
		}
		JsonObject data;
		data.SetNamedValue(L"changes", JsonValue::CreateNumberValue(static_cast<double>(sqlite3_changes(db))));
		// 同样是 int64 → double：rowid 很大时会丢精度
		data.SetNamedValue(L"lastInsertRowid", JsonValue::CreateNumberValue(static_cast<double>(sqlite3_last_insert_rowid(db))));
		Json::setOk(result);
		result.SetNamedValue(L"data", data);
		return;
	}

	if (method == L"get")
	{
		sqlite3_stmt* stmt = nullptr;
		std::wstring err;
		if (!prepare(db, sql, params, stmt, err))
		{
			Json::setError(result, err);
			return;
		}
		auto rc = sqlite3_step(stmt);
		if (rc == SQLITE_DONE) // 没查到：回 null，不报错
		{
			sqlite3_finalize(stmt);
			Json::setOk(result);
			result.SetNamedValue(L"data", JsonValue::CreateNullValue());
			return;
		}
		if (rc != SQLITE_ROW)
		{
			sqlite3_finalize(stmt);
			Json::setError(result, L"查询失败：" + Util::utf8ToWide(sqlite3_errmsg(db)));
			return;
		}
		auto row = readRow(stmt);
		sqlite3_finalize(stmt);
		Json::setOk(result);
		result.SetNamedValue(L"data", row);
		return;
	}

	if (method == L"all")
	{
		sqlite3_stmt* stmt = nullptr;
		std::wstring err;
		if (!prepare(db, sql, params, stmt, err))
		{
			Json::setError(result, err);
			return;
		}
		JsonArray rows;
		int rc = SQLITE_OK;
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) rows.Append(readRow(stmt));
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE)
		{
			Json::setError(result, L"查询失败：" + Util::utf8ToWide(sqlite3_errmsg(db)));
			return;
		}
		Json::setOk(result);
		result.SetNamedValue(L"data", rows);
		return;
	}

	Json::setError(result, L"unknown method: " + method);
}

void Db::removeByPage(Page* target)
{
	for (auto it = conns.begin(); it != conns.end();)
	{
		if (it->second->page == target) it = conns.erase(it);
		else ++it;
	}
}

void Db::removeByFrame(Page* target, ICoreWebView2Frame2* targetFrame)
{
	for (auto it = conns.begin(); it != conns.end();)
	{
		auto& conn = *it->second;
		if (conn.page == target && conn.frame.Get() == targetFrame) it = conns.erase(it);
		else ++it;
	}
}

#include "MysqlDb.h"

#include "mysql.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace ZORM {

	namespace Mysql {

		// ─────────────────────────────────────────────────────────────────
		// Connection: IDbConnection around a raw MYSQL*
		// ─────────────────────────────────────────────────────────────────

		class MysqlDb::Connection final : public IDbConnection {
		public:
			Connection(MYSQL* handle, bool parameterized, bool logClose)
				: h_(handle), parameterized_(parameterized), logClose_(logClose) {}
			~Connection() override {
				mysql_close(h_);
			}

			Json execQuery(const string& aQuery, const vector<string>& fields, Json& values) override {
				if (parameterized_)
					return execStmtQuery(aQuery, values);
				return execPlainQuery(aQuery);
			}

			Json execNone(const string& aQuery, Json& values) override {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				if (parameterized_) {
					MYSQL_STMT* stmt = mysql_stmt_init(h_);
					if (mysql_stmt_prepare(stmt, aQuery.c_str(), aQuery.length())) {
						rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errorText()));
						mysql_stmt_close(stmt);
						return rs;
					}
					std::vector<char*> dataInputs;
					MYSQL_BIND* bind = bindValues(stmt, values, dataInputs);
					if (bind == nullptr && values.size() != 0) {
						rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errorText()));
						mysql_stmt_close(stmt);
						return rs;
					}
					if (mysql_stmt_execute(stmt)) {
						rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errorText()));
						freeBind(bind, dataInputs);
						mysql_stmt_close(stmt);
						return rs;
					}
					const long long affected = static_cast<long long>(mysql_affected_rows(h_));
					rs.add("affected", affected);
					freeBind(bind, dataInputs);
					mysql_stmt_close(stmt);
				} else {
					if (mysql_query(h_, aQuery.c_str())) {
						rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errorText()));
						return rs;
					}
					const long long affected = static_cast<long long>(mysql_affected_rows(h_));
					rs.add("affected", affected);
				}
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			bool execTx(const string& aQuery, Json& values, string* out) override {
				MYSQL_STMT* stmt = mysql_stmt_init(h_);
				if (mysql_stmt_prepare(stmt, aQuery.c_str(), aQuery.length())) {
					if (out)
						*out += errorText();
					mysql_stmt_close(stmt);
					return false;
				}
				std::vector<char*> dataInputs;
				MYSQL_BIND* bind = bindValues(stmt, values, dataInputs);
				if (bind == nullptr && values.size() != 0) {
					if (out)
						*out += errorText();
					mysql_stmt_close(stmt);
					return false;
				}
				if (mysql_stmt_execute(stmt)) {
					if (out)
						*out += errorText();
					freeBind(bind, dataInputs);
					mysql_stmt_close(stmt);
					return false;
				}
				freeBind(bind, dataInputs);
				mysql_stmt_close(stmt);
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				return true;
			}

			bool beginTx() override {
				return mysql_query(h_, "begin;") == 0;
			}
			bool commitTx() override {
				return mysql_query(h_, "commit;") == 0;
			}
			void rollbackTx() override {
				mysql_query(h_, "rollback;");
			}

			void escapeLiteral(std::string& text) override {
				char* tStr = new char[text.length() * 2 + 1];
				mysql_real_escape_string(h_, tStr, text.c_str(), text.length());
				text = std::string(tStr);
				delete[] tStr;
			}

		private:
			std::string errorText() {
				std::string errmsg((char*)mysql_error(h_));
				errmsg += ". error code: ";
				errmsg += DbUtils::IntTransToString(mysql_errno(h_));
				return errmsg;
			}

			// Binds all values as strings (the wire format the other backends
			// use too); returns nullptr when binding fails.
			MYSQL_BIND* bindValues(MYSQL_STMT* stmt, Json& values, std::vector<char*>& dataInputs) {
				const int vLen = values.size();
				if (vLen <= 0)
					return nullptr;
				MYSQL_BIND* bind = new MYSQL_BIND[vLen];
				std::memset(bind, 0, sizeof(MYSQL_BIND) * vLen);
				dataInputs.resize(vLen);
				for (int i = 0; i < vLen; i++) {
					string ele = values[i].toString();
					const int eleLen = static_cast<int>(ele.length()) + 1;
					dataInputs[i] = new char[eleLen];
					std::memset(dataInputs[i], 0, eleLen);
					std::memcpy(dataInputs[i], ele.c_str(), eleLen);
					bind[i].buffer_type = MYSQL_TYPE_STRING;
					bind[i].buffer = (void*)dataInputs[i];
					bind[i].buffer_length = eleLen - 1;
				}
				if (mysql_stmt_bind_param(stmt, bind)) {
					delete[] bind;
					for (auto el : dataInputs)
						delete[] el;
					dataInputs.clear();
					return nullptr;
				}
				return bind;
			}

			static void freeBind(MYSQL_BIND* bind, std::vector<char*>& dataInputs) {
				if (bind != nullptr)
					delete[] bind;
				for (auto el : dataInputs)
					delete[] el;
			}

			// Plain (non-parameterized) query; NULL-aware decoding (SQL NULL
			// arrives as a NULL row entry - reading it via atof() would crash).
			Json execPlainQuery(const string& aQuery) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				if (mysql_query(h_, aQuery.c_str())) {
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errorText()));
					return rs;
				}
				MYSQL_RES* result = mysql_use_result(h_);
				if (result == nullptr) {
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errorText()));
					return rs;
				}
				const int num_fields = mysql_num_fields(result);
				MYSQL_FIELD* fs = mysql_fetch_fields(result);
				Json arr(JsonType::Array);
				MYSQL_ROW row;
				while ((row = mysql_fetch_row(result)) != nullptr) {
					Json al;
					for (int i = 0; i < num_fields; ++i) {
						if (row[i] == nullptr)
							al.add(fs[i].name, nullptr);
						else if (IS_NUM(fs[i].type))
							al.add(fs[i].name, atof(row[i]));
						else
							al.add(fs[i].name, Json::str(row[i]));
					}
					arr.push_back(al);
				}
				if (arr.isEmpty())
					rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
				rs.add("data", arr);
				mysql_free_result(result);
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			// Parameterized (stmt) query with typed buffer decoding.
			Json execStmtQuery(const string& aQuery, Json& values) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				MYSQL_STMT* stmt = mysql_stmt_init(h_);
				if (mysql_stmt_prepare(stmt, aQuery.c_str(), aQuery.length())) {
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errorText()));
					mysql_stmt_close(stmt);
					return rs;
				}
				std::vector<char*> dataInputs;
				MYSQL_BIND* bind = bindValues(stmt, values, dataInputs);
				if (bind == nullptr && values.size() != 0) {
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errorText()));
					mysql_stmt_close(stmt);
					return rs;
				}
				MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
				if (meta == nullptr) {
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errorText()));
					freeBind(bind, dataInputs);
					mysql_stmt_close(stmt);
					return rs;
				}
				int ret = 1;
				mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, (void*)&ret);
				if (mysql_stmt_execute(stmt)) {
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, errorText()));
					mysql_free_result(meta);
					freeBind(bind, dataInputs);
					mysql_stmt_close(stmt);
					return rs;
				}
				ret = mysql_stmt_store_result(stmt);
				const int num_fields = mysql_num_fields(meta);
				MYSQL_FIELD* fs = mysql_fetch_fields(meta);
				MYSQL_BIND* ps = new MYSQL_BIND[num_fields];
				std::memset(ps, 0, sizeof(MYSQL_BIND) * num_fields);
				std::vector<char*> dataOuts(num_fields);
				std::vector<char> isNull(num_fields, 0);
				for (int i = 0; i < num_fields; ++i) {
					const auto buf = allocateBufferForField(fs[i]);
					dataOuts[i] = new char[buf.size];
					std::memset(dataOuts[i], 0, buf.size);
					ps[i].buffer_type = buf.type;
					ps[i].buffer = (void*)dataOuts[i];
					ps[i].buffer_length = buf.size;
					ps[i].is_null = &isNull[i];
				}
				ret = mysql_stmt_bind_result(stmt, ps);
				Json arr(JsonType::Array);
				while (mysql_stmt_fetch(stmt) != MYSQL_NO_DATA) {
					Json al;
					for (int i = 0; i < num_fields; ++i) {
						if (isNull[i])
							al.add(fs[i].name, nullptr);
						// Decoding must match the declared buffer width.
						else if (fs[i].type == MYSQL_TYPE_TINY)
							al.add(fs[i].name, (long long)*(signed char*)dataOuts[i]);
						else if (fs[i].type == MYSQL_TYPE_SHORT || fs[i].type == MYSQL_TYPE_YEAR)
							al.add(fs[i].name, (long long)*(short*)dataOuts[i]);
						else if (fs[i].type == MYSQL_TYPE_LONG || fs[i].type == MYSQL_TYPE_INT24)
							al.add(fs[i].name, (long long)*(int*)dataOuts[i]);
						else if (fs[i].type == MYSQL_TYPE_LONGLONG)
							al.add(fs[i].name, *(long long*)dataOuts[i]);
						else if (fs[i].type == MYSQL_TYPE_FLOAT) {
							float f = 0.0f;
							std::memcpy(&f, dataOuts[i], sizeof(float));
							al.add(fs[i].name, (double)f);
						} else if (fs[i].type == MYSQL_TYPE_DOUBLE)
							al.add(fs[i].name, *((double*)dataOuts[i]));
						else if (fs[i].type == MYSQL_TYPE_DECIMAL || fs[i].type == MYSQL_TYPE_NEWDECIMAL)
							// DECIMAL arrives in string form in the binary
							// protocol - reading it as double yields garbage.
							al.add(fs[i].name, atof(dataOuts[i]));
						else if (fs[i].type == MYSQL_TYPE_DATETIME || fs[i].type == MYSQL_TYPE_TIMESTAMP || fs[i].type == MYSQL_TYPE_DATE) {
							MYSQL_TIME t;
							std::memcpy(&t, dataOuts[i], sizeof(MYSQL_TIME));
							char buf[48] = {0};
							if (fs[i].type == MYSQL_TYPE_DATE)
								std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.year, t.month, t.day);
							else
								std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
											  t.year, t.month, t.day, t.hour, t.minute, t.second);
							al.add(fs[i].name, Json::str(buf));
						} else if (fs[i].type == MYSQL_TYPE_TIME) {
							MYSQL_TIME t;
							std::memcpy(&t, dataOuts[i], sizeof(MYSQL_TIME));
							char buf[24] = {0};
							std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.hour, t.minute, t.second);
							al.add(fs[i].name, Json::str(buf));
						} else
							al.add(fs[i].name, Json::str(dataOuts[i]));
					}
					arr.push_back(al);
				}
				if (arr.isEmpty())
					rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
				rs.add("data", arr);
				delete[] ps;
				for (auto el : dataOuts)
					delete[] el;
				mysql_free_result(meta);
				freeBind(bind, dataInputs);
				mysql_stmt_close(stmt);
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				return rs;
			}

			struct BufferSpec {
				size_t size;
				enum_field_types type;
				BufferSpec(size_t s, enum_field_types t) : size(s + 1), type(t) {}
			};

			static BufferSpec allocateBufferForField(const MYSQL_FIELD& field) {
				switch (field.type) {
				case MYSQL_TYPE_NULL:
					return BufferSpec(0, field.type);
				case MYSQL_TYPE_TINY:
					return BufferSpec(1, field.type);
				case MYSQL_TYPE_SHORT:
					return BufferSpec(2, field.type);
				case MYSQL_TYPE_INT24:
				case MYSQL_TYPE_LONG:
				case MYSQL_TYPE_FLOAT:
					return BufferSpec(4, field.type);
				case MYSQL_TYPE_DOUBLE:
				case MYSQL_TYPE_LONGLONG:
					return BufferSpec(8, field.type);
				case MYSQL_TYPE_YEAR:
					return BufferSpec(2, MYSQL_TYPE_SHORT);
				case MYSQL_TYPE_TIMESTAMP:
				case MYSQL_TYPE_DATE:
				case MYSQL_TYPE_TIME:
				case MYSQL_TYPE_DATETIME:
					return BufferSpec(sizeof(MYSQL_TIME), field.type);
				case MYSQL_TYPE_TINY_BLOB:
				case MYSQL_TYPE_MEDIUM_BLOB:
				case MYSQL_TYPE_LONG_BLOB:
				case MYSQL_TYPE_BLOB:
				case MYSQL_TYPE_STRING:
				case MYSQL_TYPE_VAR_STRING:
				case MYSQL_TYPE_JSON:
					return BufferSpec(field.max_length, field.type);
				case MYSQL_TYPE_DECIMAL:
				case MYSQL_TYPE_NEWDECIMAL:
					return BufferSpec(64, field.type);
				case MYSQL_TYPE_BIT:
					return BufferSpec(8, MYSQL_TYPE_BIT);
				case MYSQL_TYPE_GEOMETRY:
					return BufferSpec(field.max_length, MYSQL_TYPE_BIT);
				default:
					return BufferSpec(field.max_length, field.type);
				}
			}

			MYSQL* h_;
			bool parameterized_;
			bool logClose_;
		};

		// ─────────────────────────────────────────────────────────────────
		// MysqlDb
		// ─────────────────────────────────────────────────────────────────

		MysqlDb::MysqlDb(string dbhost, string dbuser, string dbpwd, string dbname, int dbport, Json options) :
			dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(dbname), dbport(dbport),
			pool(
				[this](string& err) -> IDbConnection* { return this->newConnection(err); },
				[](IDbConnection* c) { delete c; },
				2) {
			if (!options["db_conn"].isError() && options["db_conn"].toInt() > 2)
				maxConn = options["db_conn"].toInt();
			if (!options["db_char"].isError())
				charsetName = options["db_char"].toString();
			if (!options["DbLogClose"].isError())
				DbLogClose = options["DbLogClose"].toBool();
			if (!options["parameterized"].isError())
				queryByParameter = options["parameterized"].toBool();
			if (!options["db_ssl_key"].isError())
				sslKey = options["db_ssl_key"].toString();
			if (!options["db_ssl_cert"].isError())
				sslCert = options["db_ssl_cert"].toString();
			if (!options["db_ssl_ca"].isError())
				sslCa = options["db_ssl_ca"].toString();
			if (!options["db_ssl_capath"].isError())
				sslCapath = options["db_ssl_capath"].toString();
			if (!options["db_ssl_cipher"].isError())
				sslCipher = options["db_ssl_cipher"].toString();
			if (!options["db_ssl_verify"].isError())
				sslVerify = options["db_ssl_verify"].toBool();
			if (!options["db_ssl_required"].isError())
				sslRequired = options["db_ssl_required"].toBool();
			pool.setMaxConn(maxConn);
		}

		MysqlDb::~MysqlDb() = default;

		SqlBackendBase::Lease MysqlDb::acquireConnection(string& err) {
			return pool.acquire(err);
		}

		// Upsert parity (O-6): duplicate ids update the row.
		std::string MysqlDb::upsertClause(const std::string& constraint, const std::vector<std::string>& keys) {
			(void)constraint;
			std::string clause;
			for (const std::string& k : keys) {
				if (k == "id")
					continue;
				if (!clause.empty())
					clause += ",";
				clause += k + " = " + excludedRefImpl(k);
			}
			return clause.empty() ? "" : " on duplicate key update " + clause;
		}

		std::string MysqlDb::excludedRefImpl(const std::string& column) {
			return "values(" + column + ")";
		}

		bool MysqlDb::escapeString(string& pStr) {
			string err;
			IDbConnection* conn = acquireConnection(err).get();
			if (conn == nullptr) {
				// No connection to resolve the charset against: fall back
				// to plain MySQL-compatible literal escaping instead of
				// dereferencing a NULL handle.
				string escaped;
				escaped.reserve(pStr.length() * 2 + 1);
				for (char ch : pStr) {
					switch (ch) {
					case 0: escaped += "\\0"; break;
					case '\n': escaped += "\\n"; break;
					case '\r': escaped += "\\r"; break;
					case '\\': escaped += "\\\\"; break;
					case '\'': escaped += "\\'"; break;
					case '"': escaped += "\\\""; break;
					case 26: escaped += "\\Z"; break;
					default: escaped += ch; break;
					}
				}
				pStr = escaped;
				return true;
			}
			// mysql_real_escape_string needs the charset of an established
			// connection, so the escaping runs on the leased connection.
			conn->escapeLiteral(pStr);
			return true;
		}

		IDbConnection* MysqlDb::newConnection(string& err) {
			MYSQL* mysql = mysql_init((MYSQL*)nullptr);
			if (mysql == nullptr)
				return nullptr;
			if (!charsetName.empty())
				mysql_options(mysql, MYSQL_SET_CHARSET_NAME, charsetName.c_str());
			// TLS/SSL, default ssl-mode=PREFERRED: mysql_ssl_set() switches
			// the client into TLS negotiation - WITHOUT it MariaDB
			// Connector/C connects in plain text even to TLS-capable
			// servers. NULL arguments keep client-side certificates
			// optional; the db_ssl_ca/cert/key/... options apply real
			// files when set. NOTE: only set ENFORCE when actually
			// required - setting it to 0 clears the TLS flag again.
			const bool sslEnabled =
				!sslKey.empty() || !sslCert.empty() || !sslCa.empty() ||
				!sslCapath.empty() || !sslCipher.empty();
			if (sslEnabled) {
				mysql_ssl_set(mysql,
							  sslKey.empty() ? nullptr : sslKey.c_str(),
							  sslCert.empty() ? nullptr : sslCert.c_str(),
							  sslCa.empty() ? nullptr : sslCa.c_str(),
							  sslCapath.empty() ? nullptr : sslCapath.c_str(),
							  sslCipher.empty() ? nullptr : sslCipher.c_str());
			}
			char verifyFlag = sslVerify ? 1 : 0;
			mysql_options(mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verifyFlag);
			if (sslRequired) {
				char enforceFlag = 1;
				mysql_options(mysql, MYSQL_OPT_SSL_ENFORCE, &enforceFlag);
			}
			if (mysql_real_connect(mysql, dbhost.c_str(), dbuser.c_str(), dbpwd.c_str(), dbname.c_str(), dbport, nullptr, 0))
				return new Connection(mysql, queryByParameter, DbLogClose);
			err = (char*)mysql_error(mysql);
			std::cout << "Error message : " << err;
			mysql_close(mysql);
			return nullptr;
		}

	}

}

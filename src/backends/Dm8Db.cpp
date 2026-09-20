#include "Dm8Db.h"

#include "DPI.h"
#include "DPIext.h"
#include "DPItypes.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace ZORM {

	namespace Dm8 {

		// Raw DPI connection triple (env/con/stmt). The statement handle is
		// borrowed per call from the owning connection.
		struct Dm8Con {
			dhenv henv;
			dhcon hcon;
			dhstmt hstmt;
			Dm8Con() { henv = nullptr; hcon = nullptr; hstmt = nullptr; }
		};

		// ─────────────────────────────────────────────────────────────────
		// Connection: IDbConnection around a Dm8Con (DPI env + con + stmt)
		// ─────────────────────────────────────────────────────────────────

		class Dm8Db::Connection final : public IDbConnection {
		public:
			Connection(Dm8Con* con, bool parameterized, bool logClose)
				: h_(con), parameterized_(parameterized), logClose_(logClose) {}
			~Connection() override {
				dpi_logout(h_->hcon);
				dpi_free_con(h_->hcon);
				dpi_free_env(h_->henv);
				delete h_;
			}

			Json execQuery(const string& aQuery, const vector<string>& fields, Json& values) override {
				(void)fields;
				// The DM8 path executes through the same decode routine for
				// both flavors (parameterized: prepare+bind; plain:
				// exec_direct) - one row-decoding implementation, no dup.
				return queryDecode(aQuery, values);
			}

			Json execNone(const string& aQuery, Json& values) override {
				if (parameterized_)
					return prepareExec(aQuery, values, nullptr);
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err;
				dpi_alloc_stmt(h_->hcon, &h_->hstmt);
				DPIRETURN rt = dpi_exec_direct(h_->hstmt, (sdbyte*)aQuery.c_str());
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				if (!DSQL_SUCCEEDED(rt)) {
					dpiErr(h_->hstmt, err);
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, err));
					dpi_free_stmt(h_->hstmt);
					return rs;
				}
				dpi_free_stmt(h_->hstmt);
				return rs;
			}

			bool execTx(const string& aQuery, Json& values, string* out) override {
				Json rs = prepareExec(aQuery, values, out);
				return rs["status"].toInt() == STSUCCESS;
			}

			bool beginTx() override {
				// DM8: switch the session to manual commit for the transaction.
				return DSQL_SUCCEEDED(dpi_set_con_attr(h_->hcon, DSQL_ATTR_AUTOCOMMIT, 0, 0));
			}
			bool commitTx() override {
				const bool ok = DSQL_SUCCEEDED(dpi_commit(h_->hcon));
				dpi_set_con_attr(h_->hcon, DSQL_ATTR_AUTOCOMMIT, (dpointer)1, 0);
				return ok;
			}
			void rollbackTx() override {
				dpi_rollback(h_->hcon);
				dpi_set_con_attr(h_->hcon, DSQL_ATTR_AUTOCOMMIT, (dpointer)1, 0);
			}

		private:
			void dpiErr(dhstmt hstmt, string& err) {
				sdint4 err_code;
				sdint2 msg_len;
				sdbyte err_msg[SDBYTE_MAX];
				char buf[SDBYTE_MAX];
				dpi_get_diag_rec(DSQL_HANDLE_STMT, hstmt, 1, &err_code, err_msg, sizeof(err_msg), &msg_len);
				printf("err_msg = %s, err_code = %d\n", err_msg, err_code);
				snprintf(buf, sizeof(buf), "err_msg = %s, err_code = %d\n", err_msg, err_code);
				err = string(buf);
			}

			// Values bind as NCHAR strings; numbers bind as DOUBLE / INT
			// depending on their decimal part (DM8 protocol quirk).
			DPIRETURN bindParams(dhstmt hstmt, Json& values,
								 std::vector<char*>& dataInputs,
								 std::vector<slength>& inPtrs,
								 std::vector<double>& inDbs,
								 std::vector<int>& inInts) {
				const int vLen = values.size();
				dataInputs.resize(vLen);
				inPtrs.resize(vLen);
				inDbs.resize(vLen);
				inInts.resize(vLen);
				DPIRETURN rt = DSQL_SUCCESS;
				for (int i = 0; i < vLen; i++) {
					if (values[i].isString() || values[i].isObject() || values[i].isArray()) {
						string ele = values[i].toString();
						const int eleLen = static_cast<int>(ele.length()) + 1;
						dataInputs[i] = new char[eleLen];
						std::memset(dataInputs[i], 0, eleLen);
						std::memcpy(dataInputs[i], ele.c_str(), eleLen);
						inPtrs[i] = eleLen - 1;
						rt = dpi_bind_param(hstmt, i + 1,
											DSQL_PARAM_INPUT, DSQL_C_NCHAR, DSQL_VARCHAR,
											inPtrs[i], 0, (void*)dataInputs[i], inPtrs[i], &inPtrs[i]);
					} else {
						if (getDecimalCount(values[i].toDouble()) > 0) {
							inDbs[i] = values[i].toDouble();
							inPtrs[i] = sizeof(inDbs[i]);
							rt = dpi_bind_param(hstmt, i + 1,
												DSQL_PARAM_INPUT, DSQL_C_DOUBLE, DSQL_DOUBLE,
												inPtrs[i], 0, &inDbs[i], inPtrs[i], &inPtrs[i]);
						} else {
							inInts[i] = values[i].toInt();
							inPtrs[i] = sizeof(inInts[i]);
							rt = dpi_bind_param(hstmt, i + 1,
												DSQL_PARAM_INPUT, DSQL_C_SLONG, DSQL_INT,
												inPtrs[i], 0, &inInts[i], inPtrs[i], &inPtrs[i]);
						}
					}
				}
				return rt;
			}

			// prepare + bind + exec; decodes rows when the statement returns
			// any (the DM8 path executes through here for both flavors).
			Json prepareExecDecode(const string& aQuery, Json& values) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err;
				dpi_alloc_stmt(h_->hcon, &h_->hstmt);
				DPIRETURN rt = dpi_prepare(h_->hstmt, (sdbyte*)aQuery.c_str());
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				if (!DSQL_SUCCEEDED(rt)) {
					dpiErr(h_->hstmt, err);
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, err));
					dpi_free_stmt(h_->hstmt);
					return rs;
				}
				std::vector<char*> dataInputs;
				std::vector<slength> inPtrs;
				std::vector<double> inDbs;
				std::vector<int> inInts;
				rt = bindParams(h_->hstmt, values, dataInputs, inPtrs, inDbs, inInts);
				rt = dpi_exec(h_->hstmt);
				if (!DSQL_SUCCEEDED(rt)) {
					dpiErr(h_->hstmt, err);
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, err));
					dpi_free_stmt(h_->hstmt);
					for (auto el : dataInputs)
						delete[] el;
					return rs;
				}

				Json arr = decodeRows(h_->hstmt, rt, rs, err, &dataInputs);
				return rs;
			}

			// Shared row decoding: describe + bind + fetch every column of the
			// statement. Used by both the prepared and the direct-execution
			// query paths (was duplicated in the old header-only class).
			Json decodeRows(dhstmt hstmt, DPIRETURN rt, Json& rs, string& err,
							std::vector<char*>* dataInputs) {
				sdint2 num_fields = 0;
				rt = dpi_number_columns(hstmt, &num_fields);
				std::vector<sdbyte*> fieldNames(num_fields);
				std::vector<sdint2> fieldType(num_fields);
				std::vector<sdint2> nameLen(num_fields);
				std::vector<slength> outPtrs(num_fields);
				std::vector<char*> dataOuts(num_fields);
				std::vector<double> outDoubles(num_fields);
				std::vector<int> outInts(num_fields);
				for (int i = 0; i < num_fields; ++i) {
					ulength col_sz;
					sdint2 dec_digits;
					sdint2 nullable;
					fieldNames[i] = new sdbyte[SDBYTE_MAX];
					std::memset(fieldNames[i], 0, SDBYTE_MAX);
					rt = dpi_desc_column(hstmt, i + 1, fieldNames[i], SDBYTE_MAX, &nameLen[i],
										 &fieldType[i], &col_sz, &dec_digits, &nullable);
					if (!DSQL_SUCCEEDED(rt)) {
						dpiErr(hstmt, err);
						rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, err));
						dpi_free_stmt(hstmt);
						for (auto el : fieldNames)
							delete[] el;
						if (dataInputs)
							for (auto el : *dataInputs)
								delete[] el;
						return Json();
					}
					if (fieldType[i] == DSQL_DOUBLE)
						rt = dpi_bind_col(hstmt, i + 1, DSQL_C_DOUBLE, &outDoubles[i], sizeof(double), &outPtrs[i]);
					else if (fieldType[i] == DSQL_INT)
						rt = dpi_bind_col(hstmt, i + 1, DSQL_C_SLONG, &outInts[i], sizeof(int), &outPtrs[i]);
					else {
						dataOuts[i] = new char[SDINT2_MAX];
						std::memset(dataOuts[i], 0, SDINT2_MAX);
						rt = dpi_bind_col(hstmt, i + 1, DSQL_C_NCHAR, dataOuts[i], SDINT2_MAX, &outPtrs[i]);
					}
				}

				Json arr(JsonType::Array);
				ulength row_num;
				while (dpi_fetch(hstmt, &row_num) != DSQL_NO_DATA) {
					Json al;
					for (int i = 0; i < num_fields; ++i) {
						// NULL-aware decoding: a negative indicator length
						// (DM8's NULL marker) becomes a real JSON null.
						if (outPtrs[i] < 0) {
							al.add(string((char*)(fieldNames[i])), nullptr);
							continue;
						}
						if (fieldType[i] == DSQL_DOUBLE)
							al.add(string((char*)(fieldNames[i])), outDoubles[i]);
						else if (fieldType[i] == DSQL_INT)
							al.add(string((char*)(fieldNames[i])), outInts[i]);
						else {
							string tmp(dataOuts[i]);
							al.add(string((char*)(fieldNames[i])), Json::str(tmp.erase(tmp.find_last_not_of(" ") + 1)));
						}
					}
					arr.push_back(al);
				}
				if (arr.isEmpty())
					rs.extend(DbUtils::MakeJsonObject(STQUERYEMPTY));
				rs.add("data", arr);
				dpi_free_stmt(hstmt);
				for (auto el : dataOuts)
					delete[] el;
				for (auto el : fieldNames)
					delete[] el;
				if (dataInputs)
					for (auto el : *dataInputs)
						delete[] el;
				return arr;
			}

			// Query via direct execution (non-parameterized path) - same
			// decoding as the prepared path.
			Json execDirectQuery(const string& aQuery) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err;
				dpi_alloc_stmt(h_->hcon, &h_->hstmt);
				DPIRETURN rt = dpi_exec_direct(h_->hstmt, (sdbyte*)aQuery.c_str());
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				if (!DSQL_SUCCEEDED(rt)) {
					dpiErr(h_->hstmt, err);
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, err));
					dpi_free_stmt(h_->hstmt);
					return rs;
				}
				decodeRows(h_->hstmt, rt, rs, err, nullptr);
				return rs;
			}

			Json queryDecode(const string& aQuery, Json& values) {
				if (parameterized_)
					return prepareExecDecode(aQuery, values);
				return execDirectQuery(aQuery);
			}

			// prepare + bind + exec without decoding (write path).
			Json prepareExec(const string& aQuery, Json& values, string* out) {
				Json rs = DbUtils::MakeJsonObject(STSUCCESS);
				string err;
				dpi_alloc_stmt(h_->hcon, &h_->hstmt);
				DPIRETURN rt = dpi_prepare(h_->hstmt, (sdbyte*)aQuery.c_str());
				if (!logClose_)
					std::cout << "SQL: " << aQuery << std::endl;
				if (!DSQL_SUCCEEDED(rt)) {
					dpiErr(h_->hstmt, err);
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, err));
					dpi_free_stmt(h_->hstmt);
					return rs;
				}
				std::vector<char*> dataInputs;
				std::vector<slength> inPtrs;
				std::vector<double> inDbs;
				std::vector<int> inInts;
				bindParams(h_->hstmt, values, dataInputs, inPtrs, inDbs, inInts);
				rt = dpi_exec(h_->hstmt);
				if (!DSQL_SUCCEEDED(rt)) {
					dpiErr(h_->hstmt, err);
					rs.extend(DbUtils::MakeJsonObject(STDBOPERATEERR, err));
					dpi_free_stmt(h_->hstmt);
					for (auto el : dataInputs)
						delete[] el;
					return rs;
				}
				dpi_free_stmt(h_->hstmt);
				for (auto el : dataInputs)
					delete[] el;
				return rs;
			}

			int getDecimalCount(double data) {
				data = std::abs(data);
				data -= (int)data;
				int ct = 0;
				double minValue = 0.0000000001;
				while (!(std::abs(data - 1) < minValue || std::abs(data) < minValue) && ct < 11) {
					data *= 10;
					data -= (int)data;
					ct++;
					minValue *= 10;
				}
				return ct;
			}

			Dm8Con* h_;
			bool parameterized_;
			bool logClose_;
		};

		// ─────────────────────────────────────────────────────────────────
		// Dm8Db
		// ─────────────────────────────────────────────────────────────────

		Dm8Db::Dm8Db(string dbhost, string dbuser, string dbpwd, Json options) :
			dbhost(dbhost), dbuser(dbuser), dbpwd(dbpwd), dbname(""), dbport(5236),
			pool(
				[this](string& err) -> IDbConnection* { return this->newConnection(err); },
				[](IDbConnection* c) { delete c; },
				1),
			maxConn(1) {
			if (!options["db_conn"].isError() && options["db_conn"].toInt() > 1)
				maxConn = options["db_conn"].toInt();
			if (!options["db_char"].isError())
				charsetName = options["db_char"].toString();
			if (!options["db_name"].isError())
				dbname = options["db_name"].toString();
			if (!options["db_port"].isError())
				dbport = options["db_port"].toInt();
			if (!options["DbLogClose"].isError())
				DbLogClose = options["DbLogClose"].toBool();
			if (!options["parameterized"].isError())
				queryByParameter = options["parameterized"].toBool();
			pool.setMaxConn(maxConn);
		}

		Dm8Db::~Dm8Db() = default;

		SqlBackendBase::Lease Dm8Db::acquireConnection(string& err) {
			return pool.acquire(err);
		}

		// ─────────────────────────────────────────────────────────────────
		// Dialect: quoted-lowercase identifier group
		// ─────────────────────────────────────────────────────────────────

		// DM8 folds unquoted identifiers to upper case; the generated SQL
		// always quotes lower-case names so they match the quoted DDL.
		std::string Dm8Db::quoteIdent(const std::string& name) {
			return "\"" + name + "\"";
		}
		std::string Dm8Db::qualifiedTable(const std::string& name) {
			return "\"" + dbname + "\".\"" + name + "\"";
		}
		std::string Dm8Db::likeColumn(const std::string& name) {
			return quoteIdent(name);
		}
		// "total desc" -> "\"total\" desc"; "age asc,name" -> "\"age\" asc,\"name\""
		std::string Dm8Db::orderClause(const std::string& sort) {
			vector<string> clauses = DbUtils::MakeVector(sort, ',');
			string out;
			for (const string& clause : clauses) {
				vector<string> words = DbUtils::MakeVector(clause, ' ');
				string part;
				bool firstWord = true;
				for (const string& word : words) {
					if (word.empty())
						continue;
					if (!part.empty())
						part += " ";
					part += firstWord ? quoteIdent(word) : word;
					firstWord = false;
				}
				if (part.empty())
					continue;
				if (!out.empty())
					out += ",";
				out += part;
			}
			return out.empty() ? sort : out;
		}
		std::string Dm8Db::aggColumn(const std::string& src) {
			// "1" / "*" are not column names; quote real columns so they
			// match lower-case-quoted table definitions.
			if (src == "1" || src == "*")
				return src;
			return quoteIdent(src);
		}
		std::string Dm8Db::aggAlias(const std::string& alias) {
			return quoteIdent(alias);
		}
		std::string Dm8Db::countAliasSql() {
			return quoteIdent(countAlias_);
		}
		std::string Dm8Db::columnList(const std::vector<std::string>& keys) {
			return DbUtils::GetVectorJoinStrArroundQuots(keys);
		}
		std::string Dm8Db::fieldsProjection(const vector<string>& fields) {
			return DbUtils::GetVectorJoinStrArroundQuots(fields);
		}
		// create() / insertBatch() below implement the read-then-write
		// upsert, so the SQL-level upsert clause is never used.
		std::string Dm8Db::upsertClause(const std::string&, const std::vector<std::string>&) {
			return "";
		}

		// ─────────────────────────────────────────────────────────────────
		// Upsert overrides (O-6)
		// ─────────────────────────────────────────────────────────────────

		Json Dm8Db::create(const string& tablename, const Json& params) {
			if (params.isError())
				return DbUtils::MakeJsonObject(STPARAMERR);
			if (params.isArray()) {
				if (params.size() == 0)
					return DbUtils::MakeJsonObject(STPARAMERR);
				if (params.size() > 1)
					return insertBatch(tablename, params, "id");
				return create(tablename, params[0]);
			}
			// Upsert parity: a provided id overwrites the row via a
			// read-then-update/insert (see the class comment).
			const Json providedId = params["id"];
			const bool hasId = !providedId.isError() && !DbUtils::Trim(providedId.toString()).empty();
			if (hasId) {
				Json check = select(tablename, Json{{"id", providedId.toString()}});
				if (check["status"].toInt() == STSUCCESS) {
					Json updateResult = update(tablename, params);
					if (updateResult["status"].toInt() != STSUCCESS)
						return updateResult;
					updateResult.add("id", providedId.toString());
					updateResult.add("insertId", providedId.toString());
					updateResult.add("affectedRows", 1);
					return updateResult;
				}
			}
			string sql;
			Json values(JsonType::Array);
			string generatedId;
			if (!buildInsertSql(tablename, params, sql, values, generatedId))
				return DbUtils::MakeJsonObject(STPARAMERR);
			string connectErr;
			Lease lease = acquireConnection(connectErr);
			IDbConnection* conn = lease.get();
			if (conn == nullptr)
				return DbUtils::MakeJsonObject(STDBCONNECTERR, connectErr);
			Json rs = conn->execNone(sql, values);
			const string id = generatedId.empty() ? params["id"].toString() : generatedId;
			if (!id.empty()) {
				rs.add("id", id);
				rs.add("insertId", id);
			}
			rs.add("affectedRows", 1);
			return rs;
		}

		Json Dm8Db::insertBatch(const string& tablename, const Json& elements, string constraint) {
			if (!elements.isArray() || elements.size() < 1)
				return DbUtils::MakeJsonObject(STPARAMERR);
			// Upsert parity (O-6): route every row through create()'s
			// read-then-write upsert.
			long long affected = 0;
			Json lastInsertId(0);
			for (int i = 0; i < elements.size(); ++i) {
				Json rowResult = create(tablename, elements[i]);
				if (rowResult["status"].toInt() != STSUCCESS)
					return rowResult;
				affected += rowResult["affectedRows"].isError() ? 1 : rowResult["affectedRows"].toInt();
				const Json id = rowResult["insertId"];
				if (!id.isError())
					lastInsertId = id;
			}
			Json rs = DbUtils::MakeJsonObject(STSUCCESS);
			rs.add("affectedRows", affected);
			rs.add("insertId", lastInsertId);
			return rs;
		}

		// ─────────────────────────────────────────────────────────────────
		// Driver
		// ─────────────────────────────────────────────────────────────────

		IDbConnection* Dm8Db::newConnection(string& err) {
			Dm8Con* dmCon = new Dm8Con;
			dpi_alloc_env(&dmCon->henv);
			dpi_alloc_con(dmCon->henv, &dmCon->hcon);
			string theHost = dbhost;
			theHost.append(":").append(DbUtils::IntTransToString(dbport));
			DPIRETURN rt = dpi_login(dmCon->hcon, (sdbyte*)theHost.c_str(), (sdbyte*)dbuser.c_str(), (sdbyte*)dbpwd.c_str());
			if (!DSQL_SUCCEEDED(rt)) {
				sdint4 err_code;
				sdint2 msg_len;
				sdbyte err_msg[SDBYTE_MAX];
				char buf[SDBYTE_MAX];
				dpi_get_diag_rec(DSQL_HANDLE_DBC, dmCon->hcon, 1, &err_code, err_msg, sizeof(err_msg), &msg_len);
				snprintf(buf, sizeof(buf), "err_msg = %s, err_code = %d\n", err_msg, err_code);
				err = string(buf);
				std::cout << "Error message : " << err;
				dpi_free_con(dmCon->hcon);
				dpi_free_env(dmCon->henv);
				delete dmCon;
				return nullptr;
			}
			return new Connection(dmCon, queryByParameter, DbLogClose);
		}

	}

}

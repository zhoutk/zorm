#include "gtest/gtest.h"
#include "Idb.h"
#include "DbBase.h"
#include "zjson.hpp"

TEST(TestTest, test_test_1) {
	DbBase* db = new DbBase("./db.db");
	Json qObj;
	qObj.addSubitem("id", 1);
	Json rs = db->select("users", qObj);
	EXPECT_EQ(rs["status"].toString(), "200");
}
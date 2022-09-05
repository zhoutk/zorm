﻿#pragma once
#include "zjson.hpp"
#include <sstream>
#include <time.h>
#include "GlobalConstants.h"
#include <algorithm>

class DbUtils {
public:
	static bool FindStartsStringFromVector(vector<string> strs, string value) {
		bool rs = false;
		size_t len = strs.size();
		for (size_t i = 0; i < len; i++) {
			size_t vlen = value.size();
			string key = strs[i];
			size_t klen = key.size();
			if (vlen <= klen)
				continue;
			else {
				if (value.substr(0, klen).compare(key) == 0)
				{
					rs = true;
					break;
				}
			}
		}
		return rs;
	}

	static bool FindStartsCharArray(char** strings, char* value) {
		char* string;
		char* parValue;
		while ((string = *strings++) != NULL)
		{
			parValue = value;
			while (*string != '\0' || *parValue != '\0')
			{
				if (*string == *parValue++)
				{
					string++;
					continue;
				}
				else if (*string == '\0') {
					return true;
				}
				else {
					break;
				}
				parValue++;
			}
		}
		return false;
	}

	static bool FindStringFromVector(vector<string> strs, string value) {
		auto iter = std::find(strs.begin(), strs.end(), value);
		if (iter == strs.end()) {
			return false;
		}
		else {
			return true;
		}
	}

	static bool FindCharArray(char** strings, char* value) {
		char* string;     
		char* parValue;  
		while ((string = *strings++) != NULL)
		{
			parValue = value;
			while (*string != '\0' && *parValue != '\0') 
			{
				if (*string++ == *parValue++)  
				{
					if((*parValue == '\0'))
						return true;
				}
				else {
					break;
				}
			}
		}
		return false;
	}

	static vector<string> MakeVectorInitFromString(string str, char flag = ',') {
		vector<string> rs;
		std::istringstream iss(str);
		string temp;

		while (std::getline(iss, temp, flag)) {
			rs.push_back(temp);
		}
		return rs;
	}

	static string IntTransToString(int val) {
		std::stringstream ss;
		ss << val;
		return ss.str();
	}

	static string GetVectorJoinStr(vector<string> v) {
		std::stringstream ss;
		for (size_t i = 0; i < v.size(); ++i)
		{
			if (i != 0)
				ss << ",";
			ss << v[i];
		}
		return ss.str();
	}

	static Json MakeJsonObjectForFuncReturn(StatusCodes code, string info = "") {
		Json rs;
		rs.addSubitem("code", (int)code);
		if (!info.empty()) {
			info.insert(0, "\r\ndetails, ");
		}
		info.insert(0, STCODEMESSAGES[(int)code]);
		rs.addSubitem("msg", info);
		return rs;
	}

};


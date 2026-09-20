#pragma once

// Public status codes + message table (part of the external API surface).
// The message map itself lives in GlobalConstants.cpp so every translation
// unit shares one copy instead of a per-TU static.

#include <map>
#include <string>

enum StatusCodes {
	STSUCCESS = 200,              //操作成功
	STQUERYEMPTY = 202,           //查无结果
	STPARAMERR = 301,             //输入参数错误
	STNOTFOUNDERR = 404,          //获取资源不存在
	STUPLOADERR = 411,            //文件上传失败
	STJWTAUTHERR = 421,           //授权错误或失效
	STPASSWORDERR = 422,          //密码错误
	STUSERNAMEERR = 423,          //用户名错误或不存在
	STAUTHORIZATIONERR = 431,     //权限不够
	STUSERNOTFOUNDERR = 432,      //用户不存在
	STEXCEPTIONERR = 500,         //发生异常
	STDBCONNECTERR = 700,         //数据库连接失败
	STDBOPERATEERR = 701,         //数据库操作失败
	STDBNEEDIDERR = 702,          //数据库表必须包含ID字段
	STDBNEEDRESTARTERR = 703,     //数据库表修改，需要服务重启
	STPARENTNOTFOUNDERR = 801,    //父记录不存在
};

// Message lookup for a StatusCodes value (never empty).
const std::map<int, std::string>& StatusMessages();

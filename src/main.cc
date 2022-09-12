#include "Idb.h"
#include "DbBase.h"
#include "zjson.hpp"

using namespace ZJSON;

int main()
{
	Json obj("   \r\n{\"id\":\"a2b3c4d5\",\"name\":\"test001\",\"age\":19,\"score\":69.15}");
	std::cout << "type : " << obj.getValueType() << " ; content : " << obj.toString();

	std::cout << "this is the last line.";

    return 0;
}
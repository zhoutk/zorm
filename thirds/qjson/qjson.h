#ifndef QJSON_H
#define QJSON_H

#include <QVariant>
#include <QJsonDocument>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QFile>
#include <iostream>

namespace QJSON {
	enum class JsonType
	{
		Object = 6,
		Array = 7
	};

	class Json final {
		public:
		template <typename T> Json& add(const std::string& name, const T& value)  //object add not nullptr or Json
		{
			if (this->type == Type::Object) {
				QJsonObject json = this->_obj_->object();
				if (json.contains(QString::fromStdString(name)))
					json.remove(QString::fromStdString(name));
				json.insert(QString::fromStdString(name), QJsonValue::fromVariant(value));
				this->_obj_->setObject(json);
			}
			return *this;
		};

		Json& add(const std::string& name, const std::string& value) {
			if (this->type == Type::Object) {
				QJsonObject json = this->_obj_->object();
				if (json.contains(QString::fromStdString(name)))
					json.remove(QString::fromStdString(name));
				json.insert(QString::fromStdString(name), QJsonValue::fromVariant(value.c_str()));
				this->_obj_->setObject(json);
			}
			return *this;
		}
		Json& add(const std::string& name, const std::nullptr_t&) {
			if (this->type == Type::Object) {
				QJsonObject json = this->_obj_->object();
				if (json.contains(QString::fromStdString(name)))
					json.remove(QString::fromStdString(name));
				json.insert(QString::fromStdString(name), QJsonValue::Null);
				this->_obj_->setObject(json);
			}
			return *this;
		}
		Json& add(const std::string& name, const Json& value) {
			if (this->type == Type::Object) {
				if (value.type == Type::Array || value.type == Object) {
					QJsonObject json = this->_obj_->object();
					if (json.contains(QString::fromStdString(name)))
						json.remove(QString::fromStdString(name));
					QJsonParseError json_error;
					QJsonDocument jsonDocument = QJsonDocument::fromJson(value.toString().c_str(), &json_error);
					if (json_error.error == QJsonParseError::NoError) {
						json.insert(QString::fromStdString(name), QJsonValue::fromVariant(QVariant(jsonDocument)));
						this->_obj_->setObject(json);
					}
				}
				else {
					this->remove(name);
					this->ExtendItem(name, value);
				}
			}
			return *this;
		}

		template<typename T> Json& add(const T& value) {			//Array add not nullptr or Json
			if (this->type == Type::Array) {
				QJsonArray json = this->_obj_->array();
				json.push_back(QJsonValue::fromVariant(value));
				this->_obj_->setArray(json);
			}
			return *this;
		}

		Json& add(const std::string& value) {
			if (this->type == Type::Array) {
				QJsonArray json = this->_obj_->array();
				json.push_back(QJsonValue::fromVariant(value.c_str()));
				this->_obj_->setArray(json);
			}
			return *this;
		}

		Json& add(const std::nullptr_t&) {
			if (this->type == Type::Array) {
				QJsonArray json = this->_obj_->array();
				json.push_back(QJsonValue::Null);
				this->_obj_->setArray(json);
			}
			return *this;
		}

		Json& add(const Json& value) {
			if (this->type == Type::Array) {
				if (value.type == Type::Array || value.type == Object) {
					QJsonArray json = this->_obj_->array();
					QJsonParseError json_error;
					QJsonDocument jsonDocument = QJsonDocument::fromJson(value.toString().c_str(), &json_error);
					if (json_error.error == QJsonParseError::NoError) {
						json.push_back(QJsonValue::fromVariant(QVariant(jsonDocument)));
						this->_obj_->setArray(json);
					}
				}
				else {
					this->ExtendItem(value);
				}
			}
			return *this;
		}

		Json& add(std::initializer_list<Json> values) {
			if (this->type == Type::Array) 
				for (const Json& al : values)
					this->ExtendItem(al);
			return *this;
		}

		Json() : Json(JsonType::Object) {}

		Json(JsonType type) : type((Type)type) {
			if (type == JsonType::Array || type == JsonType::Object)
				_obj_ = new QJsonDocument();
			else
				_obj_ = nullptr;
		}

		Json(std::nullptr_t) {
			new (this)Json();
			this->type = Type::Null;
		}

		Json(bool data) {
			new (this)Json();
			this->type = data ? Type::True : Type::False;
		}

		Json(char data) {
			new (this)Json((int)data);
		}

		Json(int data) {
			new (this)Json((double)data);
		}

		Json(long data) {
			new (this)Json((double)data);
		}

		Json(long long data) {
			new (this)Json((double)data);
		}

		Json(float data) {
			new (this)Json((double)data);
		}

		Json(double data) {
			new (this)Json();
			this->type = Type::Number;
			vdata = QString::number(data, 'f', getDecimalCount(data));
		}

		Json(const char* data) {
			QJsonParseError json_error;
			QJsonDocument* jsonDocument = new QJsonDocument(QJsonDocument::fromJson(data, &json_error));
			if (json_error.error == QJsonParseError::NoError) {
				_obj_ = jsonDocument;
				this->type = jsonDocument->isObject() ? Type::Object : Type::Array;
			}
			else {
				_obj_ = nullptr;
				this->type = Type::String;
				vdata = data;
			}
		}

		Json(std::string data) {
			new (this)Json(data.c_str());
		}

		Json(std::initializer_list<std::pair<const std::string, Json>> values) {
			new (this)Json();
			for (std::pair<const std::string, Json> al : values) {
				this->add(al.first, al.second);
			}
		}

		Json(const Json& origin) {
			this->type = origin.type;
			this->vdata = origin.vdata;
			if (origin._obj_)
				this->_obj_ = new QJsonDocument(*origin._obj_);
			else
				this->_obj_ = nullptr;
		}

		Json(Json&& rhs) {
			this->type = rhs.type;
			this->_obj_ = rhs._obj_;
			this->vdata = rhs.vdata;
			rhs._obj_ = nullptr;
		}

		Json& operator = (const Json& origin) {
			return *(new (this)Json(origin));
		}

		bool operator != (const Json& right) {
			return
				this->type != right.type ||
				this->_obj_ != right._obj_ ||
				this->vdata.compare(right.vdata) != 0;
		}

		bool operator == (const Json& right) {
			return
				this->type == right.type &&
				this->_obj_ == right._obj_ &&
				this->vdata.compare(right.vdata) == 0;
		}

		Json& operator = (Json&& rhs) {
			return *(new (this)Json(std::move(rhs)));
		}

		Json operator[](const int& index) const {
			Json rs(Type::Error);
			if (this->type == Type::Array) {
				QJsonArray arr = this->_obj_->array();
				if (index >= 0 && index < arr.size()) {
					QJsonValue v = arr[index];
					parseValueToJson(rs, v);
				}
			}
			return rs;
		}

		Json operator[](const std::string& key) const {
			Json rs(Type::Error);
			if (this->type == Type::Object) {
				QJsonObject obj = this->_obj_->object();
				QJsonValue v = obj[QString::fromStdString(key)];
				parseValueToJson(rs, v);
			}
			return rs;
		}

		std::string toString() const {
			switch (this->type)
			{
			case Type::Array:
			case Type::Object:
				return this->_obj_->toJson(QJsonDocument::Compact);
			case Type::Number:
			case Type::String:
				return this->vdata.toStdString();
			case Type::True:
				return "true";
			case Type::False:
				return "false";
		/*	case Type::Null:
				return "null";*/
			default:
				return "";
			}
		}

		bool contains(const std::string& key) const {
			Json f = (*this)[key];
			return !(f.type == Type::Null || f.type == Type::Error);
		}

		std::string getValueType() const {
			return TYPENAMES[this->type].toStdString();
		}

		Json take(const std::string& key) {		//get and remove of Object
			if (this->type == Type::Object) {
				Json rs = (*this)[key];
				if ((rs.type != Type::Null && rs.type != Type::Error)) 
					(*this).remove(key);
				return rs;
			}else
				return Json(Type::Error);
		}

		Json take(const int& index) {		//get and remove of Array
			if (this->type == Type::Array) {
				Json rs(Type::Error);
				if (index >= 0 && index < this->size()) {
					rs = (*this)[index];
					(*this).remove(index);
				}
				return rs;
			}
			else
				return Json(Type::Error);
		}

		Json first() const {
			if (this->type == Type::Array && this->size() > 0)
				return (*this)[0];
			else
				return Json(Type::Error);
		}

		Json last() const {
			if (this->type == Type::Array && this->size() > 0)
				return (*this)[this->size() - 1];
			else
				return Json(Type::Error);
		}

		std::vector<std::string> getAllKeys() const {
			std::vector<std::string> rs;
			if (this->type == Type::Object) {
				QJsonObject obj = this->_obj_->object();
				for (QString al : obj.keys())
					rs.push_back(al.toStdString());
			}
			return rs;
		}

		Json& extend(const Json& value) {
			if (this->type == Type::Object) {
				if (value.type == Type::Object) {
					QJsonObject tobj = this->_obj_->object();
					QJsonObject obj = value._obj_->object();
					QJsonObject::const_iterator it = obj.constBegin();
					QJsonObject::const_iterator end = obj.constEnd();
					while (it != end)
					{
						tobj.insert(it.key(), it.value());
						it++;
					}
					this->_obj_->setObject(tobj);
				}
			}
			return *this;
		}

		Json& concat(const Json& value) {
			if (this->type == Type::Array) {
				if (value.type == Type::Array) {
					QJsonArray tarr = this->_obj_->array();
					QJsonArray varr = value._obj_->array();
					for (int i = 0; i < varr.size(); i++) 
						tarr.push_back(varr[i]);
					this->_obj_->setArray(tarr);
				}
				else if (value.type == Type::Object) {
					QJsonArray tarr = this->_obj_->array();
					QJsonObject obj = value._obj_->object();
					QJsonObject::const_iterator it = obj.constBegin();
					QJsonObject::const_iterator end = obj.constEnd();
					while (it != end)
					{
						tarr.push_back(it.value());
						it++;
					}
					this->_obj_->setArray(tarr);
				}
				else {
					this->ExtendItem(value);
				}
			}
			return *this;
		}

		Json& push_back(const Json& value) {
			if (this->type == Type::Array)
				return this->add(value);
			return *this;
		}
		inline Json& push(const Json& value) {
			return this->push_back(value);
		}

		Json& push_front(const Json& value) {
			if (this->type == Type::Array)
				return this->insert(0, value);
			return *this;
		}

		Json& pop_back() {
			if (this->type == Type::Array)
				this->removeLast();
			return *this;
		}
		inline Json& pop() {
			return this->pop_back();
		}

		Json& pop_front() {
			if (this->type == Type::Array)
				return this->removeFirst();
			return *this;
		}

		Json slice(int start, int end = 0) const {
			Json rs(Type::Array);
			if (this->type == Type::Array) {
				if (end == 0)
					end = this->size();
				while (start < end)
					rs.push_back((*this)[start++]);
			}
			return rs;
		}

		Json takes(int start, int end = 0) {
			Json rs(Type::Array);
			if (this->type == Type::Array) {
				if (end == 0)
					end = this->size();
				while (start < end) {
					rs.push_back(this->take(start));
					end--;
				}
			}
			return rs;
		}

		Json& insert(const int& index, const Json& value) {
			if (this->type == Type::Array && index >= 0 && index <= this->size()) {
				if (value.type == Type::Array || value.type == Object) {
					QJsonArray arr = this->_obj_->array();
					QJsonParseError json_error;
					QJsonDocument jsonDocument = QJsonDocument::fromJson(value.toString().c_str(), &json_error);
					if (json_error.error == QJsonParseError::NoError) {
						arr.insert(index, QJsonValue::fromVariant(QVariant(jsonDocument)));
						this->_obj_->setArray(arr);
					}
				}
				else {
					Json rear = this->takes(index);
					this->ExtendItem(value);
					for (int i = 0; i < rear.size(); i++) 
						this->push_back(rear[i]);
				}
			}
			return *this;
		}

		int size() const {
			if (this->type == Type::Array) 
				return this->_obj_->array().size();
			else
				return 0;
		}

		bool isError() const {
			return this->type == Type::Error;
		}

		bool isNull() const {
			return this->type == Type::Null;
		}

		bool isObject() const {
			return this->type == Type::Object;
		}

		bool isArray() const {
			return this->type == Type::Array;
		}

		bool isNumber() const {
			return this->type == Type::Number;
		}

		bool isTrue() const {
			return this->type == Type::True;
		}

		bool isFalse() const {
			return this->type == Type::False;
		}

		bool isString() const {
			return this->type == Type::String;
		}

		bool isEmpty() const {
			return this->size() <= 0;
		}

		float toFloat() const {
			return (float)this->toDouble();
		}

		int toInt() const {
			return (int)this->toDouble();
		}

		double toDouble() const {
			return this->vdata.toDouble();
		}

		bool toBool() const {
			if (this->type == Type::True)
				return true;
			else
				return false;
		}

		std::vector<Json> toVector() const {
			std::vector<Json> rs;
			if (this->type == Type::Array) 
				for (int i = 0; i < this->size(); i++)
					rs.push_back((*this)[i]);
			return rs;
		}

		Json& clear() {
			if (this->type == Type::Array) {
				QJsonArray arr = this->_obj_->array();
				while (!arr.isEmpty())
					arr.removeLast();
				this->_obj_->setArray(arr);
			} 
			else if(this->type == Type::Object) {
				QJsonObject json = this->_obj_->object();
				QJsonObject::iterator iter = json.begin();
				while (iter != json.end())
					json.remove(iter.key());
				this->_obj_->setObject(json);
			}
			else
				this->vdata.clear();
			return *this;
		}

		Json& remove(const std::string& name) {
			if (this->type == Type::Object && this->_obj_) {
				QJsonObject json = this->_obj_->object();
				if (json.contains(QString::fromStdString(name)))
					json.remove(QString::fromStdString(name));
				this->_obj_->setObject(json);
			}
			return *this;
		}

		Json& remove(const int& index) {
			if (this->type == Type::Array) {
				if (index >= 0 && index < this->size()) {
					QJsonArray json = this->_obj_->array();
					json.removeAt(index);
					this->_obj_->setArray(json);
				}
			}
			return *this;
		}

		Json& removeFirst() {
			if (this->type == Type::Array) {
				this->remove(0);
			}
			return *this;
		}

		Json& removeLast() {
			if (this->type == Type::Array) {
				this->remove(this->size() - 1);
			}
			return *this;
		}

		static Json FromFile(const char* filepath) {
			QFile file(filepath);
			if (file.open(QIODevice::ReadOnly))
				return Json(std::string(file.readAll()));
			else
				return Json(Type::Error);
		}

		static Json FromFile(const std::string& filepath) {
			return Json::FromFile(filepath.c_str());
		}

		~Json() {
			if (_obj_)
				delete _obj_;
		}

		private:
		enum Type {
			Error,
			False,
			True,
			Null,
			Number,
			String,
			Object,
			Array
		};
		QStringList TYPENAMES {"Error", "False", "True", "Null", "Number", "String", "Object", "Array"};
		Type type;
		QJsonDocument* _obj_;
		QString vdata;


		Json(Type type) {
			if (type == Object || type == Array) {
				this->_obj_ = new QJsonDocument;
			}
			else {
				this->_obj_ = nullptr;
			}
			this->type = type;
		}

		void parseValueToJson(Json& rs, const QJsonValue& v) const {
			switch (v.type())
			{
			case QJsonValue::Array:
				rs.type = Type::Array;
				rs._obj_ = new QJsonDocument(v.toArray());
				break;
			case QJsonValue::Object:
				rs.type = Type::Object;
				rs._obj_ = new QJsonDocument(v.toObject());
				break;
			case QJsonValue::Double:
				rs.type = Type::Number;
				rs._obj_ = nullptr;
				rs.vdata = QString::number(v.toDouble(), 'f', getDecimalCount(v.toDouble()));
				break;
			case QJsonValue::String:
				{
					QJsonParseError json_error;
					QJsonDocument* jsonDocument = new QJsonDocument(QJsonDocument::fromJson(v.toString().toUtf8(), &json_error));
					if (json_error.error == QJsonParseError::NoError) {
						delete rs._obj_;
						rs._obj_ = jsonDocument;
						rs.type = jsonDocument->isObject() ? Type::Object : Type::Array;
					}
					else {
						rs.type = Type::String;
						rs.vdata = v.toString();
					}
				}
				break;
			case QJsonValue::Bool:
				rs.type = v.toBool() ? Type::True : Type::False;
				rs._obj_ = nullptr;
				break;
			case QJsonValue::Null:
				rs.type = Type::Null;
				rs._obj_ = nullptr;
				break;
			default:
				rs.type = Type::Error;
				rs._obj_ = nullptr;
				break;
			}
		}

		int getDecimalCount(double data) const {
			data = qAbs(data);
			data -= (int)data;
			int ct = 0;
			double minValue = 0.0000000001;
			while (!(qAbs(data - 1) < minValue || qAbs(data) < minValue) && ct < 11) {
				data *= 10;
				data -= (int)data;
				ct++;
				minValue *= 10;
			}
			return ct;
		}

		void ExtendItem(const std::string& name, const Json& cur) {			//Object
			switch (cur.type)
			{
			case Type::False:
				this->add(name, false);
				break;
			case Type::True:
				this->add(name, true);
				break;
			case Type::Null:
				this->add(name, nullptr);
				break;
			case Type::Number:
				this->add(name, cur.vdata.toDouble());
				break;
			case Type::String:
				this->add(name, cur.vdata);
				break;
			case Type::Object:
			case Type::Array:
				this->add(name, cur);
				break;
			default:
				break;
			}
		}

		void ExtendItem(const Json& cur) {				//Array
			switch (cur.type)
			{
			case Type::False:
				this->add(false);
				break;
			case Type::True:
				this->add(true);
				break;
			case Type::Null:
				this->add(nullptr);
				break;
			case Type::Number:
				this->add(cur.vdata.toDouble());
				break;
			case Type::String:
				this->add(cur.vdata);
				break;
			case Type::Object:
			case Type::Array:
				this->add(cur);
				break;
			default:
				break;
			}
		}

	};

}

#endif // QJSON_H
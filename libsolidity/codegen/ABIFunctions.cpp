/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @author Christian <chris@ethereum.org>
 * @date 2017
 * Routines that generate JULIA code related to ABI encoding, decoding and type conversions.
 */

#include <libsolidity/codegen/ABIFunctions.h>

#include <libdevcore/Whiskers.h>

#include <libsolidity/ast/AST.h>

using namespace std;
using namespace dev;
using namespace dev::solidity;

ABIFunctions::~ABIFunctions()
{
	// This throws an exception and thus might cause immediate termination, but hey,
	// it's a failed assertion anyway :-)
//TODO	solAssert(m_requestedFunctions.empty(), "Forgot to call ``requestedFunctions()``.");
}

string ABIFunctions::tupleEncoder(
	TypePointers const& _givenTypes,
	TypePointers const& _targetTypes,
	bool _encodeAsLibraryTypes
)
{
	// stack: <$value0> <$value1> ... <$value(n-1)> <$headStart>

	string encoder = R"(
		let dynFree := add($headStart, <headSize>)
		<#values>
			dynFree := <abiEncode>(
				$value<i>,
				$headStart,
				add($headStart, <headPos>),
				dynFree
			)
		</values>
		$value0 := dynFree
	)";
	solAssert(!_givenTypes.empty(), "");
	size_t headSize = 0;
	for (auto const& t: _targetTypes)
	{
		solAssert(t->calldataEncodedSize() > 0, "");
		headSize += t->calldataEncodedSize();
	}
	Whiskers templ(encoder);
	templ("headSize", to_string(headSize));
	vector<Whiskers::StringMap> values(_givenTypes.size());
	map<string, pair<TypePointer, TypePointer>> requestedEncodingFunctions;
	size_t headPos = 0;
	for (size_t i = 0; i < _givenTypes.size(); ++i)
	{
		solUnimplementedAssert(_givenTypes[i]->sizeOnStack() == 1, "");
		solAssert(_givenTypes[i], "");
		solAssert(_targetTypes[i], "");
		values[i]["fromTypeID"] = _givenTypes[i]->identifier();
		values[i]["toTypeID"] = _targetTypes[i]->identifier();
		values[i]["i"] = to_string(i);
		values[i]["headPos"] = to_string(headPos);
		values[i]["abiEncode"] =
			abiEncodingFunction(*_givenTypes[i], *_targetTypes[i], _encodeAsLibraryTypes);
		headPos += _targetTypes[i]->calldataEncodedSize();
	}
	solAssert(headPos == headSize, "");
	templ("values", values);

	return templ.render();
}

string ABIFunctions::requestedFunctions()
{
	string result;
	for (auto const& f: m_requestedFunctions)
		result += f.second;
	m_requestedFunctions.clear();
	return result;
}

string ABIFunctions::cleanupFunction(Type const& _type, bool _revertOnFailure)
{
	string functionName = string("cleanup_") + (_revertOnFailure ? "revert_" : "assert_") + _type.identifier();
	if (!m_requestedFunctions.count(functionName))
	{
		Whiskers templ(R"(
			function <functionName>(value) -> cleaned {
				<body>
			}
		)");
		templ("functionName", functionName);
		switch (_type.category())
		{
		case Type::Category::Integer:
		{
			IntegerType const& type = dynamic_cast<IntegerType const&>(_type);
			if (type.numBits() == 256)
				templ("body", "cleaned := value");
			else if (type.isSigned())
				templ("body", "cleaned := signextend(" + to_string(type.numBits() / 8 - 1) + ", value)");
			else
				templ("body", "cleaned := and(value, 0x" + toHex((u256(1) << type.numBits()) - 1) + ")");
			break;
		}
		case Type::Category::Bool:
			templ("body", "cleaned := iszero(iszero(value))");
			break;
		case Type::Category::FixedPoint:
			solUnimplemented("Fixed point types not implemented.");
			break;
		case Type::Category::Array:
			solAssert(false, "Array cleanup requested.");
			break;
		case Type::Category::Struct:
			solAssert(false, "Struct cleanup requested.");
			break;
		case Type::Category::FixedBytes:
		{
			FixedBytesType const& type = dynamic_cast<FixedBytesType const&>(_type);
			if (type.numBytes() == 32)
				templ("body", "cleaned := value");
			else
			{
				size_t numBits = type.numBytes() * 8;
				u256 mask = ((u256(1) << numBits) - 1) << (256 - numBits);
				templ("body", "cleaned := and(value, 0x" + toHex(mask) + ")");
			}
			break;
		}
		case Type::Category::Contract:
			templ("body", "cleaned := " + cleanupFunction(IntegerType(120, IntegerType::Modifier::Address)) + "(value)");
			break;
		case Type::Category::Enum:
		{
			size_t members = dynamic_cast<EnumType const&>(_type).numberOfMembers();
			Whiskers w("switch lt(value, <members>) case 0 { <failure> }");
			w("members", to_string(members));
			if (_revertOnFailure)
				w("failure", "revert(0, 0)");
			else
				w("failure", "invalid()");
			templ("body", w.render());
			break;
		}
		default:
			solAssert(false, "Cleanup of type " + _type.identifier() + " requested.");
		}

		m_requestedFunctions[functionName] = templ.render();
	}
	return functionName;
}

string ABIFunctions::abiEncodingFunction(
	Type const& _givenType,
	Type const& _targetType,
	bool _encodeAsLibraryTypes
)
{
	string functionName =
		"abi_encode_" +
		_givenType.identifier() +
		"_to_" +
		_targetType.identifier() +
		(_encodeAsLibraryTypes ? "_lib" : "");
	if (!m_requestedFunctions.count(functionName))
	{
		Whiskers templ(R"(
			function <functionName>(value, headStart, headPos, dyn) -> newDyn {
				<body>
			}
		)");
		templ("functionName", functionName);

		string body;
		if (_targetType.isDynamicallySized())
		{
			solUnimplementedAssert(false, "");
		}
		else
		{
			body = "newDyn := dyn\n";
			solUnimplementedAssert(_givenType.sizeOnStack() == 1, "");
			if (_givenType.dataStoredIn(DataLocation::Storage) && _targetType.isValueType())
			{
				// special case: convert storage reference type to value type - this is only
				// possible for library calls where we just forward the storage reference
				solAssert(_encodeAsLibraryTypes, "");
				solAssert(_givenType.sizeOnStack() == 1, "");
				solAssert(_targetType == IntegerType(256), "");
				body += "mstore(headPos, value)";
			}
			else if (
				_givenType.dataStoredIn(DataLocation::Storage) ||
				_givenType.dataStoredIn(DataLocation::CallData) ||
				_givenType.category() == Type::Category::StringLiteral ||
				_givenType.category() == Type::Category::Function
			)
			{
				// This used to delay conversion
				solUnimplemented("");
			}
			else if (dynamic_cast<ArrayType const*>(&_targetType))
			{
				// This used to perform a conversion first and then call
				// ArrayUtils(m_context).copyArrayToMemory(*arrayType, _padToWordBoundaries);
				solUnimplemented("");
			}
			else
			{
				solUnimplementedAssert(_givenType == _targetType, "");
				solAssert(_targetType.calldataEncodedSize() == 32, "");
				body += "mstore(headPos, " + cleanupFunction(_givenType) + "(value))\n";
			}
		}
		templ("body", body);
		m_requestedFunctions[functionName] = templ.render();
	}

	return functionName;
}

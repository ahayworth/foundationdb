/*
 * Tenant.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FDBCLIENT_TENANT_H
#define FDBCLIENT_TENANT_H
#pragma once

#include "fdbclient/FDBTypes.h"
#include "fdbclient/VersionedMap.h"
#include "fdbclient/KeyBackedTypes.h"
#include "flow/flat_buffers.h"

typedef StringRef TenantNameRef;
typedef Standalone<TenantNameRef> TenantName;
typedef StringRef TenantGroupNameRef;
typedef Standalone<TenantGroupNameRef> TenantGroupName;

enum class TenantState { REGISTERING, READY, REMOVING, UPDATING_CONFIGURATION, ERROR };

struct TenantMapEntry {
	constexpr static FileIdentifier file_identifier = 12247338;

	static Key idToPrefix(int64_t id);
	static int64_t prefixToId(KeyRef prefix);

	static std::string tenantStateToString(TenantState tenantState);
	static TenantState stringToTenantState(std::string stateStr);

	Arena arena;
	int64_t id = -1;
	Key prefix;
	Optional<TenantGroupName> tenantGroup;
	TenantState tenantState = TenantState::READY;
	// TODO: fix this type
	Optional<Standalone<StringRef>> assignedCluster;
	int64_t configurationSequenceNum = 0;

	constexpr static int PREFIX_SIZE = sizeof(id);

	TenantMapEntry();
	TenantMapEntry(int64_t id, TenantState tenantState);
	TenantMapEntry(int64_t id, Optional<TenantGroupName> tenantGroup, TenantState tenantState);

	bool matchesConfiguration(TenantMapEntry const& other) const;
	void configure(Standalone<StringRef> parameter, Optional<Value> value);

	std::string toJson(int apiVersion) const;

	Value encode() const { return ObjectWriter::toValue(*this, IncludeVersion(ProtocolVersion::withTenantGroups())); }

	static TenantMapEntry decode(ValueRef const& value) {
		TenantMapEntry entry;
		ObjectReader reader(value.begin(), IncludeVersion());
		reader.deserialize(entry);
		return entry;
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, id, tenantGroup, tenantState, assignedCluster, configurationSequenceNum);
		if (ar.isDeserializing) {
			prefix = idToPrefix(id);
			ASSERT(tenantState >= TenantState::REGISTERING && tenantState <= TenantState::ERROR);
		}
	}
};

struct TenantMetadataSpecification {
	static KeyRef subspace;

	// TODO: can we break compatibility and use the tuple codec?
	struct TenantIdCodec {
		static Standalone<StringRef> pack(int64_t id) { return TenantMapEntry::idToPrefix(id); }
		static int64_t unpack(Standalone<StringRef> val) { return TenantMapEntry::prefixToId(val); }
	};

	KeyBackedObjectMap<TenantName, TenantMapEntry, decltype(IncludeVersion()), NullCodec> tenantMap;
	KeyBackedProperty<int64_t, TenantIdCodec> lastTenantId;
	KeyBackedSet<int64_t> tenantTombstones;
	KeyBackedSet<Tuple> tenantGroupTenantIndex;

	TenantMetadataSpecification(KeyRef subspace)
	  : tenantMap(subspace.withSuffix("tenant/map/"_sr), IncludeVersion(ProtocolVersion::withTenantGroups())),
	    lastTenantId(subspace.withSuffix("tenant/lastId"_sr)),
	    tenantTombstones(subspace.withSuffix("tenant/tombstones/"_sr)),
	    tenantGroupTenantIndex(subspace.withSuffix("tenant/tenantGroup/tenantIndex/"_sr)) {}
};

struct TenantMetadata {
private:
	static inline TenantMetadataSpecification instance = TenantMetadataSpecification("\xff/"_sr);

public:
	static inline auto& tenantMap = instance.tenantMap;
	static inline auto& lastTenantId = instance.lastTenantId;
	static inline auto& tenantTombstones = instance.tenantTombstones;
	static inline auto& tenantGroupTenantIndex = instance.tenantGroupTenantIndex;

	static inline Key tenantMapPrivatePrefix = "\xff"_sr.withSuffix(tenantMap.subspace.begin);
};

typedef VersionedMap<TenantName, TenantMapEntry> TenantMap;
typedef VersionedMap<Key, TenantName> TenantPrefixIndex;

#endif
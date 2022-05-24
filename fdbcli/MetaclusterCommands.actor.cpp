/*
 * MetaclusterCommands.actor.cpp
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

#include "fdbcli/fdbcli.actor.h"

#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/IClientApi.h"
#include "fdbclient/Knobs.h"
#include "fdbclient/MetaclusterManagement.actor.h"
#include "fdbclient/Schemas.h"

#include "flow/Arena.h"
#include "flow/FastRef.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/actorcompiler.h" // This must be the last #include.

namespace fdb_cli {

Optional<std::pair<Optional<ClusterConnectionString>, Optional<DataClusterEntry>>>
parseClusterConfiguration(std::vector<StringRef> const& tokens, DataClusterEntry const& defaults, int startIndex) {
	Optional<DataClusterEntry> entry;
	Optional<ClusterConnectionString> connectionString;

	for (int tokenNum = startIndex; tokenNum < tokens.size(); ++tokenNum) {
		StringRef token = tokens[tokenNum];
		StringRef param = token.eat("=");
		std::string value = token.toString();
		if (tokencmp(param, "max_tenant_groups")) {
			entry = defaults;

			int n;
			if (sscanf(value.c_str(), "%d%n", &entry.get().capacity.numTenantGroups, &n) != 1 || n != value.size() ||
			    entry.get().capacity.numTenantGroups < 0) {
				fprintf(stderr, "ERROR: invalid number of tenant groups %s\n", value.c_str());
				return Optional<std::pair<Optional<ClusterConnectionString>, Optional<DataClusterEntry>>>();
			}
		} else if (tokencmp(param, "connection_string")) {
			connectionString = ClusterConnectionString(value);
		} else {
			fprintf(stderr, "ERROR: unrecognized configuration parameter %s\n", param.toString().c_str());
			return Optional<std::pair<Optional<ClusterConnectionString>, Optional<DataClusterEntry>>>();
		}
	}

	return std::make_pair(connectionString, entry);
}

void printMetaclusterConfigureOptionsUsage() {
	fmt::print("max_tenant_groups sets the maximum number of tenant groups that can be assigned\n"
	           "to the named data cluster.\n");
	fmt::print("connection_string sets the connection string for the named data cluster.\n");
}

// metacluster register command
ACTOR Future<bool> metaclusterRegisterCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() < 4) {
		fmt::print("Usage: metacluster register <NAME> <max_tenant_groups=<NUM_GROUPS>|\n"
		           "connection_string=<CONNECTION_STRING>> ...\n\n");
		fmt::print("Adds a data cluster with the given connection string to a metacluster.\n");
		fmt::print("NAME is used to identify the cluster in future commands.\n");
		printMetaclusterConfigureOptionsUsage();
		return false;
	}

	DataClusterEntry defaultEntry;
	auto config = parseClusterConfiguration(tokens, defaultEntry, 3);
	if (!config.present()) {
		return false;
	} else if (!config.get().first.present()) {
		fprintf(stderr, "ERROR: connection_string must be configured when registering a cluster.\n");
		return false;
	}

	wait(MetaclusterAPI::registerCluster(
	    db, tokens[2], config.get().first.get(), config.get().second.orDefault(defaultEntry)));

	printf("The cluster `%s' has been added\n", printable(tokens[2]).c_str());
	return true;
}

// metacluster remove command
ACTOR Future<bool> metaclusterRemoveCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() < 3 || tokens.size() > 4 || (tokens.size() == 4 && tokens[2] != "FORCE"_sr)) {
		fmt::print("Usage: metacluster remove [FORCE] <NAME> \n\n");
		fmt::print("Removes the specified data cluster from a metacluster.\n");
		fmt::print("If FORCE is specified, then the cluster will be detached even if it has\n"
		           "tenants assigned to it.\n");
		return false;
	}

	state ClusterNameRef clusterName = tokens[tokens.size() - 1];
	wait(MetaclusterAPI::removeCluster(db, clusterName, tokens.size() == 4));

	printf("The cluster `%s' has been removed\n", printable(clusterName).c_str());
	return true;
}

// metacluster list command
ACTOR Future<bool> metaclusterListCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() > 6) {
		fmt::print("Usage: metacluster list [BEGIN] [END] [LIMIT]\n\n");
		fmt::print("Lists the data clusters in a metacluster.\n");
		fmt::print("Only cluster names in the range BEGIN - END will be printed.\n");
		fmt::print("An optional LIMIT can be specified to limit the number of results (default 100).\n");
		return false;
	}

	state ClusterNameRef begin = tokens.size() > 2 ? tokens[2] : ""_sr;
	state ClusterNameRef end = tokens.size() > 3 ? tokens[3] : "\xff"_sr;
	int limit = 100;

	if (tokens.size() > 4) {
		int n = 0;
		if (sscanf(tokens[3].toString().c_str(), "%d%n", &limit, &n) != 1 || n != tokens[3].size() || limit < 0) {
			fprintf(stderr, "ERROR: invalid limit %s\n", tokens[3].toString().c_str());
			return false;
		}
	}

	std::map<ClusterName, DataClusterMetadata> clusters = wait(MetaclusterAPI::listClusters(db, begin, end, limit));
	if (clusters.empty()) {
		if (tokens.size() == 2) {
			printf("The metacluster has no registered data clusters\n");
		} else {
			printf("The metacluster has no registered data clusters in the specified range\n");
		}
	}

	int index = 0;
	for (auto cluster : clusters) {
		printf("  %d. %s\n", ++index, printable(cluster.first).c_str());
	}

	return true;
}

// metacluster get command
ACTOR Future<bool> metaclusterGetCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() > 4 || (tokens.size() == 4 && tokens[3] != "JSON"_sr)) {
		fmt::print("Usage: metacluster get <NAME> [JSON]\n\n");
		fmt::print("Prints metadata associated with the given data cluster.\n");
		fmt::print("If JSON is specified, then the output will be in JSON format.\n");
		return false;
	}

	state bool useJson = tokens.size() == 4;

	try {
		DataClusterMetadata metadata = wait(MetaclusterAPI::getCluster(db, tokens[2]));

		if (useJson) {
			json_spirit::mObject obj;
			obj["type"] = "success";
			obj["cluster"] = metadata.toJson();
			printf("%s\n", json_spirit::write_string(json_spirit::mValue(obj), json_spirit::pretty_print).c_str());
		} else {
			printf("  connection string: %s\n", metadata.connectionString.toString().c_str());
			printf("  tenant group capacity: %d\n", metadata.entry.capacity.numTenantGroups);
			printf("  allocated tenant groups: %d\n", metadata.entry.allocated.numTenantGroups);
		}
	} catch (Error& e) {
		if (useJson) {
			json_spirit::mObject obj;
			obj["type"] = "error";
			obj["error"] = e.what();
			printf("%s\n", json_spirit::write_string(json_spirit::mValue(obj), json_spirit::pretty_print).c_str());
			return false;
		} else {
			throw;
		}
	}

	return true;
}

// metacluster configure command
ACTOR Future<bool> metaclusterConfigureCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() < 4) {
		fmt::print("Usage: metacluster configure <NAME> <max_tenant_groups=<NUM_GROUPS>|\n"
		           "connection_string=<CONNECTION_STRING>> ...\n\n");
		fmt::print("Updates the configuration of the metacluster.\n");
		printMetaclusterConfigureOptionsUsage();
		return false;
	}

	state Reference<ITransaction> tr = db->createTransaction();

	loop {
		try {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			Optional<DataClusterMetadata> metadata = wait(MetaclusterAPI::tryGetClusterTransaction(tr, tokens[2]));
			if (!metadata.present()) {
				throw cluster_not_found();
			}

			auto config = parseClusterConfiguration(tokens, metadata.get().entry, 3);
			if (!config.present()) {
				return false;
			}

			MetaclusterAPI::updateClusterMetadata(tr, tokens[2], config.get().first, config.get().second);

			wait(safeThreadFutureToFuture(tr->commit()));
			break;
		} catch (Error& e) {
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}

	return true;
}

// metacluster command
Future<bool> metaclusterCommand(Reference<IDatabase> db, std::vector<StringRef> tokens) {
	if (tokens.size() == 1) {
		printUsage(tokens[0]);
		return true;
	} else if (tokencmp(tokens[1], "register")) {
		return metaclusterRegisterCommand(db, tokens);
	} else if (tokencmp(tokens[1], "remove")) {
		return metaclusterRemoveCommand(db, tokens);
	} else if (tokencmp(tokens[1], "list")) {
		return metaclusterListCommand(db, tokens);
	} else if (tokencmp(tokens[1], "get")) {
		return metaclusterGetCommand(db, tokens);
	} else if (tokencmp(tokens[1], "configure")) {
		return metaclusterConfigureCommand(db, tokens);
	} else {
		printUsage(tokens[0]);
		return true;
	}
}

void metaclusterGenerator(const char* text,
                          const char* line,
                          std::vector<std::string>& lc,
                          std::vector<StringRef> const& tokens) {
	if (tokens.size() == 1) {
		const char* opts[] = { "register", "remove", "list", "get", "configure", nullptr };
		arrayGenerator(text, line, opts, lc);
	} else if (tokens.size() > 1 && (tokencmp(tokens[1], "register") || tokencmp(tokens[1], "configure"))) {
		const char* opts[] = { "max_tenant_groups=", "connection_string=", nullptr };
		arrayGenerator(text, line, opts, lc);
	}
}

std::vector<const char*> metaclusterHintGenerator(std::vector<StringRef> const& tokens, bool inArgument) {
	if (tokens.size() == 1) {
		return { "<register|remove|list|get|configure>", "[ARGS]" };
	} else if (tokencmp(tokens[1], "register")) {
		static std::vector<const char*> opts = {
			"<NAME>", "<max_tenant_groups=<NUM_GROUPS>|connection_string=<CONNECTION_STRING>>"
		};
		return std::vector<const char*>(opts.begin() + std::min<int>(1, tokens.size() - 2), opts.end());
	} else if (tokencmp(tokens[1], "remove") && tokens.size() < 4) {
		static std::vector<const char*> opts = { "[FORCE]", "<NAME>" };
		if (tokens.size() == 2) {
			return opts;
		} else if (tokens.size() == 3 && (inArgument || tokens[2].size() == "FORCE"_sr.size()) &&
		           "FORCE"_sr.startsWith(tokens[2])) {
			return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
		} else {
			return std::vector<const char*>();
		}
	} else if (tokencmp(tokens[1], "list") && tokens.size() < 5) {
		static std::vector<const char*> opts = { "[BEGIN]", "[END]", "[LIMIT]" };
		return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
	} else if (tokencmp(tokens[1], "get") && tokens.size() < 4) {
		static std::vector<const char*> opts = { "<NAME>", "[JSON]" };
		return std::vector<const char*>(opts.begin() + tokens.size() - 2, opts.end());
	} else if (tokencmp(tokens[1], "configure")) {
		static std::vector<const char*> opts = {
			"<NAME>", "<max_tenant_groups=<NUM_GROUPS>|connection_string=<CONNECTION_STRING>>"
		};
		return std::vector<const char*>(opts.begin() + std::min<int>(1, tokens.size() - 2), opts.end());
	} else {
		return std::vector<const char*>();
	}
}

CommandFactory metaclusterRegisterFactory("metacluster",
                                          CommandHelp("metacluster <register|remove|list|get|configure> [ARGS]",
                                                      "view and manage a metacluster",
                                                      "Use `register' to add a data cluster to the metacluster."),
                                          &metaclusterGenerator,
                                          &metaclusterHintGenerator);

} // namespace fdb_cli

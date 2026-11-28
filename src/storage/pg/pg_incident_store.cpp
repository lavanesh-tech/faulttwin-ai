#include "faulttwin/storage/pg_incident_store.hpp"

#include <array>
#include <charconv>
#include <string_view>
#include <system_error>

#include "faulttwin/core/json.hpp"
#include "faulttwin/core/time.hpp"

namespace faulttwin::storage {

namespace {

using collectors::SqlError;
using collectors::SqlErrorKind;
using collectors::SqlTable;

// The SQL below is built from string literals at compile time; these two macros keep the
// conversions identical everywhere, and a constexpr function cannot concatenate literals.
// $n::bigint microseconds -> timestamptz, exactly (no floating point on the way).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FT_TS(n) "(timestamptz 'epoch' + $" #n "::bigint * interval '1 microsecond')"
// timestamptz -> bigint microseconds; extract() returns numeric (exact) since PostgreSQL 14.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define FT_US(col) "(extract(epoch from " col ") * 1000000)::bigint"

/// Version 1 of the meta-db schema. Constraints repeat the C++ invariants so that a bug in
/// one writer cannot store a state the readers do not understand.
constexpr std::array<std::string_view, 4> kMigrationV1{
    R"sql(CREATE TABLE faulttwin.incidents (
    id                     bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    rule_set_version       text NOT NULL CHECK (length(rule_set_version) BETWEEN 1 AND 128),
    feature_schema_version text NOT NULL CHECK (length(feature_schema_version) BETWEEN 1 AND 128),
    rule_id                text NOT NULL CHECK (length(rule_id) BETWEEN 1 AND 128),
    node                   text NOT NULL CHECK (length(node) BETWEEN 1 AND 128),
    severity               text NOT NULL CHECK (severity IN ('warning', 'critical')),
    state                  text NOT NULL CHECK (state IN ('open', 'resolved')),
    resolution             text CHECK (resolution IN ('recovered', 'monitor_restarted')),
    violation_since        timestamptz NOT NULL,
    opened_at              timestamptz NOT NULL,
    resolved_at            timestamptz,
    updated_at             timestamptz NOT NULL,
    ack_by                 text CHECK (length(ack_by) BETWEEN 1 AND 64),
    ack_note               text CHECK (length(ack_note) <= 1024),
    ack_at                 timestamptz,
    CHECK ((state = 'open') = (resolved_at IS NULL AND resolution IS NULL)),
    CHECK ((ack_by IS NULL) = (ack_at IS NULL))
))sql",
    R"sql(CREATE UNIQUE INDEX incidents_one_open_per_alert
    ON faulttwin.incidents (rule_id, node) WHERE state = 'open')sql",
    R"sql(CREATE INDEX incidents_opened_at ON faulttwin.incidents (opened_at))sql",
    R"sql(CREATE TABLE faulttwin.incident_events (
    incident_id bigint NOT NULL REFERENCES faulttwin.incidents (id) ON DELETE CASCADE,
    seq         smallint NOT NULL CHECK (seq BETWEEN 1 AND 3),
    kind        text NOT NULL CHECK (kind IN ('fired', 'escalated', 'resolved')),
    severity    text NOT NULL CHECK (severity IN ('warning', 'critical')),
    at          timestamptz NOT NULL,
    evidence    json NOT NULL,
    PRIMARY KEY (incident_id, seq)
))sql",
};

/// Columns of every incident read; incident_from_row() expects exactly these names.
constexpr std::string_view kSelectIncident =
    "SELECT i.id, i.rule_set_version, i.feature_schema_version, i.rule_id, i.node, i.severity, "
    "i.state, i.resolution, " FT_US("i.violation_since") " AS violation_since_us, " FT_US(
        "i.opened_at") " AS opened_at_us, " FT_US("i.resolved_at") " AS resolved_at_us, " FT_US("i.updated_at") " AS "
                                                                                                "updated_at_us, "
    "i.ack_by, i.ack_note, " FT_US("i.ack_at") " AS ack_at_us, "
    "COALESCE((SELECT json_agg(json_build_array(e.seq, e.kind, e.severity, " FT_US(
        "e.at") ", e.evidence) ORDER BY e.seq) "
                "FROM faulttwin.incident_events e WHERE e.incident_id = i.id), '[]'::json) AS events "
                "FROM faulttwin.incidents i ";

bool is_connection_level(SqlErrorKind kind) {
    return kind == SqlErrorKind::kConnectionFailed || kind == SqlErrorKind::kTimeout ||
           kind == SqlErrorKind::kAuthenticationFailed;
}

StoreError from_sql(const SqlError& error) {
    if (is_connection_level(error.kind)) {
        return {StoreErrorKind::kUnavailable,
                "meta-db " + std::string(collectors::to_string(error.kind)) + ": " + error.message};
    }
    if (error.sqlstate == "23505") {
        return {StoreErrorKind::kConflict, "an incident for this rule and node is already open"};
    }
    if (error.sqlstate.starts_with("23")) {
        return {StoreErrorKind::kConflict, "constraint violation: " + error.message};
    }
    return {StoreErrorKind::kInternal, "meta-db error " + error.sqlstate + ": " + error.message};
}

std::string micros(Timestamp t) {
    return std::to_string(core::to_unix_micros(t));
}

std::optional<std::int64_t> parse_int(std::string_view text) {
    std::int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> first_cell_u64(const SqlTable& table) {
    if (table.rows.size() != 1 || table.rows[0].empty()) {
        return std::nullopt;
    }
    const auto& cell = table.rows[0][0];
    if (!cell) {
        return std::nullopt;
    }
    const auto v = parse_int(*cell);
    if (!v || *v < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*v);
}

/// The two non-NULL cells of a one-row, two-column result.
std::optional<std::pair<std::string, std::string>> two_cells(const SqlTable& table) {
    if (table.rows.size() != 1 || table.rows[0].size() != 2) {
        return std::nullopt;
    }
    const auto& a = table.rows[0][0];
    const auto& b = table.rows[0][1];
    if (!a || !b) {
        return std::nullopt;
    }
    return std::pair{*a, *b};
}

StoreError bad_result(std::string_view what) {
    return {StoreErrorKind::kInternal, "unexpected meta-db result for " + std::string(what)};
}

std::optional<DetectionKind> detection_kind_from_string(std::string_view text) {
    for (const auto k :
         {DetectionKind::kFired, DetectionKind::kEscalated, DetectionKind::kResolved}) {
        if (detection::to_string(k) == text) {
            return k;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<IncidentEvent>> events_from_json(std::string_view json) {
    core::JsonLimits limits;
    limits.max_bytes = 1024UL * 1024UL;
    auto parsed = core::parse_json(json, limits);
    const auto* root = std::get_if<core::JsonValue>(&parsed);
    if (root == nullptr || root->as_array() == nullptr) {
        return std::nullopt;
    }
    std::vector<IncidentEvent> events;
    for (const auto& item : *root->as_array()) {
        const auto* fields = item.as_array();
        if (fields == nullptr || fields->size() != 5) {
            return std::nullopt;
        }
        const auto* kind = (*fields)[1].as_string();
        const auto* severity = (*fields)[2].as_string();
        const auto* at = (*fields)[3].as_number();
        const auto* evidence = (*fields)[4].as_object();
        if (kind == nullptr || severity == nullptr || at == nullptr || evidence == nullptr) {
            return std::nullopt;
        }
        IncidentEvent event;
        const auto k = detection_kind_from_string(*kind);
        const auto s = severity_from_string(*severity);
        if (!k || !s) {
            return std::nullopt;
        }
        event.kind = *k;
        event.severity = *s;
        // json_agg renders the bigint as a JSON number; microsecond epochs are < 2^53, so the
        // double holds them exactly.
        event.at = core::from_unix_micros(static_cast<std::int64_t>(*at));
        for (const auto& [name, value] : *evidence) {
            if (value.is_null()) {
                event.evidence.push_back({name, std::nullopt});
            } else if (const auto* number = value.as_number()) {
                event.evidence.push_back({name, *number});
            } else {
                return std::nullopt;
            }
        }
        events.push_back(std::move(event));
    }
    return events;
}

}  // namespace

namespace {

/// Typed access to one result row by column name.
class RowReader {
  public:
    RowReader(const std::vector<std::string>& columns,
              const std::vector<std::optional<std::string>>& row)
        : columns_(&columns), row_(&row) {}

    [[nodiscard]] std::optional<std::string> text(std::string_view name) const {
        for (std::size_t i = 0; i < columns_->size() && i < row_->size(); ++i) {
            if ((*columns_)[i] == name) {
                return (*row_)[i];
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Timestamp> time(std::string_view name) const {
        const auto t = text(name);
        const auto v = t ? parse_int(*t) : std::nullopt;
        return v ? std::optional{core::from_unix_micros(*v)} : std::nullopt;
    }

  private:
    const std::vector<std::string>* columns_;
    const std::vector<std::optional<std::string>>* row_;
};

/// The NOT NULL text columns; false if any is missing.
bool read_identity(const RowReader& r, Incident& incident) {
    for (const auto& [name, target] : std::array<std::pair<std::string_view, std::string*>, 4>{{
             {"rule_set_version", &incident.rule_set_version},
             {"feature_schema_version", &incident.feature_schema_version},
             {"rule_id", &incident.rule_id},
             {"node", &incident.node},
         }}) {
        auto value = r.text(name);
        if (!value) {
            return false;
        }
        *target = std::move(*value);
    }
    return true;
}

/// Severity, state and resolution; false if any is unknown.
bool read_status(const RowReader& r, Incident& incident) {
    const auto severity = r.text("severity");
    const auto state = r.text("state");
    const auto sev = severity ? severity_from_string(*severity) : std::nullopt;
    const auto st = state ? incident_state_from_string(*state) : std::nullopt;
    if (!sev || !st) {
        return false;
    }
    incident.severity = *sev;
    incident.state = *st;
    if (const auto text = r.text("resolution")) {
        incident.resolution = resolution_from_string(*text);
        if (!incident.resolution) {
            return false;
        }
    }
    return true;
}

/// The three timestamps every incident has, plus the optional resolved_at.
bool read_times(const RowReader& r, Incident& incident) {
    const auto violation_since = r.time("violation_since_us");
    const auto opened_at = r.time("opened_at_us");
    const auto updated_at = r.time("updated_at_us");
    if (!violation_since || !opened_at || !updated_at) {
        return false;
    }
    incident.violation_since = *violation_since;
    incident.opened_at = *opened_at;
    incident.updated_at = *updated_at;
    incident.resolved_at = r.time("resolved_at_us");
    return true;
}

/// The acknowledgement, if any; false if it is only partly present.
bool read_acknowledgement(const RowReader& r, Incident& incident) {
    const auto by = r.text("ack_by");
    if (!by) {
        return true;
    }
    const auto at = r.time("ack_at_us");
    if (!at) {
        return false;
    }
    incident.acknowledgement = Acknowledgement{*by, r.text("ack_note").value_or(""), *at};
    return true;
}

}  // namespace

std::optional<Incident> incident_from_row(const std::vector<std::string>& columns,
                                          const std::vector<std::optional<std::string>>& row) {
    const RowReader r(columns, row);
    Incident incident;
    const auto id = r.text("id");
    const auto id_value = id ? parse_int(*id) : std::nullopt;
    if (!id_value || *id_value <= 0) {
        return std::nullopt;
    }
    incident.id = static_cast<std::uint64_t>(*id_value);
    if (!read_identity(r, incident) || !read_status(r, incident) || !read_times(r, incident) ||
        !read_acknowledgement(r, incident)) {
        return std::nullopt;
    }
    const auto events = r.text("events");
    auto parsed_events = events ? events_from_json(*events) : std::nullopt;
    if (!parsed_events) {
        return std::nullopt;
    }
    incident.events = std::move(*parsed_events);
    return incident;
}

PgIncidentStore::PgIncidentStore(SessionFactory factory) : factory_(std::move(factory)) {}

StoreResult<SqlTable> PgIncidentStore::run(std::string_view sql, const Params& params) {
    if (!session_) {
        auto connected = factory_();
        if (auto* error = std::get_if<SqlError>(&connected)) {
            return StoreError{StoreErrorKind::kUnavailable,
                              "meta-db connect failed: " + error->message};
        }
        session_ = std::move(std::get<std::unique_ptr<collectors::SqlSession>>(connected));
    }
    auto outcome = session_->query_params(sql, params);
    if (auto* error = std::get_if<SqlError>(&outcome)) {
        if (is_connection_level(error->kind)) {
            session_.reset();
        }
        return from_sql(*error);
    }
    return std::move(std::get<SqlTable>(outcome));
}

StoreResult<int> PgIncidentStore::migrate() {
    const std::scoped_lock lock(mutex_);
    const auto fail = [this](StoreError error) -> StoreResult<int> {
        if (session_) {
            (void)session_->query_params("ROLLBACK", {});
        }
        return error;
    };
    for (const std::string_view step :
         {std::string_view{"BEGIN"}, std::string_view{"SET LOCAL client_min_messages = warning"},
          std::string_view{"SELECT pg_advisory_xact_lock(hashtext('faulttwin.schema'))"},
          std::string_view{"CREATE TABLE IF NOT EXISTS faulttwin.schema_migrations ("
                           "version integer PRIMARY KEY, applied_at timestamptz NOT NULL "
                           "DEFAULT now())"}}) {
        auto r = run(step, {});
        if (auto* error = std::get_if<StoreError>(&r)) {
            return fail(*error);
        }
    }
    auto current = run("SELECT COALESCE(max(version), 0) FROM faulttwin.schema_migrations", {});
    if (auto* error = std::get_if<StoreError>(&current)) {
        return fail(*error);
    }
    const auto version = first_cell_u64(std::get<SqlTable>(current));
    if (!version) {
        return fail(bad_result("schema version"));
    }
    if (*version > static_cast<std::uint64_t>(kMetaDbSchemaVersion)) {
        return fail({StoreErrorKind::kInvalid, "meta-db schema version " +
                                                   std::to_string(*version) +
                                                   " is newer than this build (" +
                                                   std::to_string(kMetaDbSchemaVersion) + ")"});
    }
    if (*version < 1) {
        for (const auto statement : kMigrationV1) {
            auto r = run(statement, {});
            if (auto* error = std::get_if<StoreError>(&r)) {
                return fail(*error);
            }
        }
        auto r = run("INSERT INTO faulttwin.schema_migrations (version) VALUES (1)", {});
        if (auto* error = std::get_if<StoreError>(&r)) {
            return fail(*error);
        }
    }
    auto commit = run("COMMIT", {});
    if (auto* error = std::get_if<StoreError>(&commit)) {
        return fail(*error);
    }
    return kMetaDbSchemaVersion;
}

StoreResult<std::uint64_t> PgIncidentStore::open(const NewIncident& incident) {
    if (auto problem = validate_new_incident(incident); !problem.empty()) {
        return StoreError{StoreErrorKind::kInvalid, problem};
    }
    static constexpr std::string_view kSql =
        "WITH inc AS ("
        " INSERT INTO faulttwin.incidents (rule_set_version, feature_schema_version, rule_id, "
        " node, severity, state, violation_since, opened_at, updated_at)"
        " VALUES ($1, $2, $3, $4, $5, 'open', " FT_TS(6) ", " FT_TS(7) ", " FT_TS(7) ")"
        " RETURNING id)"
        " INSERT INTO faulttwin.incident_events (incident_id, seq, kind, severity, at, evidence)"
        " SELECT id, 1, 'fired', $5, " FT_TS(7) ", $8::json FROM inc RETURNING incident_id";
    const std::scoped_lock lock(mutex_);
    auto result =
        run(kSql, {incident.rule_set_version, incident.feature_schema_version, incident.rule_id,
                   incident.node, std::string(detection::to_string(incident.severity)),
                   micros(incident.violation_since), micros(incident.opened_at),
                   evidence_json(incident.evidence)});
    if (auto* error = std::get_if<StoreError>(&result)) {
        return *error;
    }
    const auto id = first_cell_u64(std::get<SqlTable>(result));
    if (!id) {
        return bad_result("open");
    }
    return *id;
}

StoreResult<Done> PgIncidentStore::append_event(std::uint64_t id, const IncidentEvent& event,
                                                std::optional<Resolution> resolution) {
    // One statement: update the open incident, append the event with the next sequence
    // number, and report whether the row exists so that "not found" and "already resolved"
    // can be told apart. The CHECK on seq refuses a fourth event.
    static constexpr std::string_view kEscalate =
        "WITH upd AS ("
        " UPDATE faulttwin.incidents SET"
        " severity = CASE WHEN $2 = 'critical' THEN 'critical' ELSE severity END,"
        " updated_at = " FT_TS(3) " WHERE id = $1::bigint AND state = 'open' RETURNING id),"
        " ev AS (INSERT INTO faulttwin.incident_events (incident_id, seq, kind, severity, at, "
        " evidence) SELECT id, (SELECT count(*) + 1 FROM faulttwin.incident_events "
        " WHERE incident_id = $1::bigint), 'escalated', $2, " FT_TS(3) ", $4::json FROM upd "
        " RETURNING 1)"
        " SELECT (SELECT count(*) FROM ev),"
        " EXISTS (SELECT 1 FROM faulttwin.incidents WHERE id = $1::bigint)";
    static constexpr std::string_view kResolve =
        "WITH upd AS ("
        " UPDATE faulttwin.incidents SET state = 'resolved', resolution = $5,"
        " resolved_at = " FT_TS(3) ", updated_at = " FT_TS(3)
        " WHERE id = $1::bigint AND state = 'open' RETURNING id),"
        " ev AS (INSERT INTO faulttwin.incident_events (incident_id, seq, kind, severity, at, "
        " evidence) SELECT id, (SELECT count(*) + 1 FROM faulttwin.incident_events "
        " WHERE incident_id = $1::bigint), 'resolved', $2, " FT_TS(3) ", $4::json FROM upd "
        " RETURNING 1)"
        " SELECT (SELECT count(*) FROM ev),"
        " EXISTS (SELECT 1 FROM faulttwin.incidents WHERE id = $1::bigint)";
    Params params{std::to_string(id), std::string(detection::to_string(event.severity)),
                  micros(event.at), evidence_json(event.evidence)};
    if (resolution) {
        params.emplace_back(std::string(to_string(*resolution)));
    }
    const std::scoped_lock lock(mutex_);
    auto result = run(resolution ? kResolve : kEscalate, params);
    if (auto* error = std::get_if<StoreError>(&result)) {
        return *error;
    }
    const auto cells = two_cells(std::get<SqlTable>(result));
    if (!cells) {
        return bad_result("event");
    }
    if (cells->first == "1") {
        return Done{};
    }
    if (cells->second == "t") {
        return StoreError{StoreErrorKind::kConflict,
                          "incident " + std::to_string(id) + " is already resolved"};
    }
    return StoreError{StoreErrorKind::kNotFound, "incident " + std::to_string(id) + " not found"};
}

StoreResult<Done> PgIncidentStore::escalate(std::uint64_t id, const IncidentEvent& event) {
    return append_event(id, event, std::nullopt);
}

StoreResult<Done> PgIncidentStore::resolve(std::uint64_t id, const IncidentEvent& event,
                                           Resolution resolution) {
    return append_event(id, event, resolution);
}

StoreResult<std::uint64_t> PgIncidentStore::close_orphans(Timestamp at) {
    static constexpr std::string_view kSql =
        "WITH closed AS (UPDATE faulttwin.incidents SET state = 'resolved',"
        " resolution = 'monitor_restarted', resolved_at = " FT_TS(1) ", updated_at = " FT_TS(
            1) " WHERE state = 'open' RETURNING 1) SELECT count(*) FROM closed";
    const std::scoped_lock lock(mutex_);
    auto result = run(kSql, {micros(at)});
    if (auto* error = std::get_if<StoreError>(&result)) {
        return *error;
    }
    const auto count = first_cell_u64(std::get<SqlTable>(result));
    if (!count) {
        return bad_result("close_orphans");
    }
    return *count;
}

StoreResult<Incident> PgIncidentStore::acknowledge(std::uint64_t id, const Acknowledgement& ack) {
    if (auto problem = validate_acknowledgement(ack); !problem.empty()) {
        return StoreError{StoreErrorKind::kInvalid, problem};
    }
    static constexpr std::string_view kSql =
        "WITH upd AS (UPDATE faulttwin.incidents SET ack_by = $2, ack_note = $3,"
        " ack_at = " FT_TS(4) " WHERE id = $1::bigint AND ack_by IS NULL RETURNING 1)"
        " SELECT (SELECT count(*) FROM upd),"
        " EXISTS (SELECT 1 FROM faulttwin.incidents WHERE id = $1::bigint)";
    const std::scoped_lock lock(mutex_);
    auto result = run(kSql, {std::to_string(id), ack.by, ack.note, micros(ack.at)});
    if (auto* error = std::get_if<StoreError>(&result)) {
        return *error;
    }
    const auto cells = two_cells(std::get<SqlTable>(result));
    if (!cells) {
        return bad_result("acknowledge");
    }
    if (cells->first != "1") {
        if (cells->second == "t") {
            return StoreError{StoreErrorKind::kConflict,
                              "incident " + std::to_string(id) + " is already acknowledged"};
        }
        return StoreError{StoreErrorKind::kNotFound,
                          "incident " + std::to_string(id) + " not found"};
    }
    return get_locked(id);
}

StoreResult<Incident> PgIncidentStore::get_locked(std::uint64_t id) {
    auto result =
        run(std::string(kSelectIncident) + "WHERE i.id = $1::bigint", {std::to_string(id)});
    if (auto* error = std::get_if<StoreError>(&result)) {
        return *error;
    }
    const auto& table = std::get<SqlTable>(result);
    if (table.rows.empty()) {
        return StoreError{StoreErrorKind::kNotFound,
                          "incident " + std::to_string(id) + " not found"};
    }
    auto incident = incident_from_row(table.columns, table.rows.front());
    if (!incident) {
        return bad_result("incident row");
    }
    return std::move(*incident);
}

StoreResult<Incident> PgIncidentStore::get(std::uint64_t id) {
    const std::scoped_lock lock(mutex_);
    return get_locked(id);
}

StoreResult<IncidentPage> PgIncidentStore::list(const IncidentQuery& query) {
    if (auto problem = validate_query(query); !problem.empty()) {
        return StoreError{StoreErrorKind::kInvalid, problem};
    }
    // A fixed statement: absent filters are NULL parameters, so no SQL is ever built from
    // request text. One extra row is fetched to know whether a next page exists.
    const std::string sql =
        std::string(kSelectIncident) +
        "WHERE ($1::text IS NULL OR i.state = $1) AND ($2::text IS NULL OR i.node = $2)"
        " AND ($3::text IS NULL OR i.rule_id = $3) AND ($4::text IS NULL OR i.severity = $4)"
        " AND ($5::bigint IS NULL OR i.opened_at >= " FT_TS(5) ")"
        " AND ($6::bigint IS NULL OR i.opened_at < " FT_TS(6) ")"
        " AND ($7::bigint IS NULL OR i.id < $7::bigint)"
        " ORDER BY i.id DESC LIMIT $8::bigint";
    const auto opt = [](const auto& value, auto fn) -> std::optional<std::string> {
        return value ? std::optional<std::string>(fn(*value)) : std::nullopt;
    };
    const Params params{
        opt(query.state, [](IncidentState s) { return std::string(to_string(s)); }),
        query.node,
        query.rule_id,
        opt(query.severity, [](Severity s) { return std::string(detection::to_string(s)); }),
        opt(query.opened_since, [](Timestamp t) { return micros(t); }),
        opt(query.opened_until, [](Timestamp t) { return micros(t); }),
        opt(query.before_id, [](std::uint64_t id) { return std::to_string(id); }),
        std::to_string(query.limit + 1),
    };
    const std::scoped_lock lock(mutex_);
    auto result = run(sql, params);
    if (auto* error = std::get_if<StoreError>(&result)) {
        return *error;
    }
    const auto& table = std::get<SqlTable>(result);
    IncidentPage page;
    for (const auto& row : table.rows) {
        if (page.items.size() == query.limit) {
            page.next_before_id = page.items.back().id;
            break;
        }
        auto incident = incident_from_row(table.columns, row);
        if (!incident) {
            return bad_result("incident row");
        }
        page.items.push_back(std::move(*incident));
    }
    return page;
}

StoreResult<StoreStats> PgIncidentStore::stats() {
    const std::scoped_lock lock(mutex_);
    auto result =
        run("SELECT count(*) FILTER (WHERE state = 'open'),"
            " count(*) FILTER (WHERE state = 'resolved') FROM faulttwin.incidents",
            {});
    if (auto* error = std::get_if<StoreError>(&result)) {
        return *error;
    }
    const auto cells = two_cells(std::get<SqlTable>(result));
    if (!cells) {
        return bad_result("stats");
    }
    const auto open = parse_int(cells->first);
    const auto resolved = parse_int(cells->second);
    if (!open || !resolved) {
        return bad_result("stats");
    }
    return StoreStats{static_cast<std::uint64_t>(*open), static_cast<std::uint64_t>(*resolved)};
}

StoreResult<Done> PgIncidentStore::ping() {
    const std::scoped_lock lock(mutex_);
    auto result = run("SELECT 1", {});
    if (auto* error = std::get_if<StoreError>(&result)) {
        return *error;
    }
    return Done{};
}

#undef FT_TS
#undef FT_US

}  // namespace faulttwin::storage

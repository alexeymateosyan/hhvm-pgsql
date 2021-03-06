#include "pgsql.h"

#include "hphp/runtime/base/zend-string.h"

#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/server/server-stats.h"
#include "hphp/runtime/ext/string/ext_string.h"

#define PGSQL_ASSOC 1
#define PGSQL_NUM 2
#define PGSQL_BOTH (PGSQL_ASSOC | PGSQL_NUM)
#define PGSQL_STATUS_LONG 1
#define PGSQL_STATUS_STRING 2

#ifdef HACK_FRIENDLY
#define FAIL_RETURN return null_variant
#else
#define FAIL_RETURN return false
#endif

namespace HPHP {

namespace { // Anonymous namespace

struct ScopeNonBlocking {
    ScopeNonBlocking(PQ::Connection& conn, bool mode) :
        m_conn(conn), m_mode(mode) {}

    ~ScopeNonBlocking() {
        m_conn.setNonBlocking(m_mode);
    }

    PQ::Connection& m_conn;
    bool m_mode;
};


class PGSQLConnectionPool;

static class PGSQLConnectionPoolContainer {
private:
    std::map<std::string, PGSQLConnectionPool*> m_pools;
    Mutex m_lock;



public:
    PGSQLConnectionPoolContainer();
    PGSQLConnectionPoolContainer(PGSQLConnectionPoolContainer const&);
    void operator=(PGSQLConnectionPoolContainer const&);

    ~PGSQLConnectionPoolContainer();

    PGSQLConnectionPool& GetPool(const std::string);
    std::vector<PGSQLConnectionPool *> &GetPools();

} s_connectionPoolContainer;



class PGSQLConnectionPool {
private:
    int m_maximumConnections;
    Mutex m_lock;
    std::string m_connectionString;
    std::string m_cleanedConnectionString;
    std::queue<PQ::Connection*> m_availableConnections;
    std::vector<PQ::Connection*> m_connections;

    long m_sweepedConnections = 0;
    long m_openedConnections = 0;
    long m_requestedConnections = 0;
    long m_releasedConnections = 0;
    long m_errors = 0;

public:



    long SweepedConnections() const { return m_sweepedConnections; }
    long OpenedConnections() const { return m_openedConnections; }
    long RequestedConnections() const { return m_requestedConnections; }
    long ReleasedConnections() const { return m_releasedConnections; }
    long Errors() const { return m_errors; }

    int TotalConnectionsCount() const { return m_connections.size(); }
    int FreeConnectionsCount() const { return m_availableConnections.size(); }

    PGSQLConnectionPool(std::string connectionString, int maximumConnections = -1);
    ~PGSQLConnectionPool();

    PQ::Connection& GetConnection();
    void Release(PQ::Connection& connection);

    std::string GetConnectionString() const { return m_connectionString; }
    std::string GetCleanedConnectionString() const { return m_cleanedConnectionString; }

    void CloseAllConnections();
    void CloseFreeConnections();
    int MaximumConnections() const { return m_maximumConnections; }
    void SweepConnection(PQ::Connection& connection);
};



class PGSQL : public SweepableResourceData {
    DECLARE_RESOURCE_ALLOCATION(PGSQL);
public:
    static bool AllowPersistent;
    static int MaxPersistent;
    static int MaxLinks;
    static bool AutoResetPersistent;
    static bool IgnoreNotice;
    static bool LogNotice;

    static PGSQL *Get(const Variant& conn_id);

public:
    PGSQL(String conninfo);
    PGSQL(PGSQLConnectionPool& connectionPool);
    ~PGSQL();

    void ReleaseConnection();

    static StaticString s_class_name;
    virtual const String& o_getClassNameHook() const { return s_class_name; }
    virtual bool isResource() const { return m_conn != nullptr; }

    PQ::Connection &get() { return *m_conn; }

    ScopeNonBlocking asNonBlocking() {
        auto mode = m_conn->isNonBlocking();
        return ScopeNonBlocking(*m_conn, mode);
    }

    bool IsConnectionPooled() const { return m_connectionPool != nullptr; }

private:
    PQ::Connection* m_conn;

    PGSQLConnectionPool* m_connectionPool = nullptr;

public:
    std::string m_conn_string;

    std::string m_db;
    std::string m_user;
    std::string m_pass;
    std::string m_host;
    std::string m_port;
    std::string m_options;

    std::string m_last_notice;
    void SetupInformation();
};

class PGSQLResult : public SweepableResourceData {
    DECLARE_RESOURCE_ALLOCATION(PGSQLResult);
public:
    static PGSQLResult *Get(const Variant& result);
public:
    PGSQLResult(PGSQL* conn, PQ::Result res);
    ~PGSQLResult();

    static StaticString s_class_name;
    virtual const String& o_getClassNameHook() const { return s_class_name; }
    virtual bool isResource() const { return (bool)m_res; }

    void close();

    PQ::Result& get() { return m_res; }

    int getFieldNumber(const Variant& field);
    int getNumFields();
    int getNumRows();

    bool convertFieldRow(const Variant& row, const Variant& field,
        int *out_row, int *out_field, const char *fn_name = nullptr);

    Variant fieldIsNull(const Variant& row, const Variant& field, const char *fn_name = nullptr);

    Variant getFieldVal(const Variant& row, const Variant& field, const char *fn_name = nullptr);
    String getFieldVal(int row, int field, const char *fn_name = nullptr);

    PGSQL * getConn() { return m_conn; }

public:
    int m_current_row;
private:
    PQ::Result m_res;
    int m_num_fields;
    int m_num_rows;
    PGSQL * m_conn;
};
}

//////////////////////////////////////////////////////////////////////////////////

StaticString PGSQL::s_class_name("pgsql connection");
StaticString PGSQLResult::s_class_name("pgsql result");


PGSQL *PGSQL::Get(const Variant& conn_id) {
    if (conn_id.isNull()) {
        return nullptr;
    }

    PGSQL *pgsql = conn_id.toResource().getTyped<PGSQL>(true, true);
    return pgsql;
}

static void notice_processor(PGSQL *pgsql, const char *message) {
    if (pgsql != nullptr) {
        pgsql->m_last_notice = message;

        if (PGSQL::LogNotice) {
            raise_notice("%s", message);
        }
    }
}


void PGSQL::SetupInformation()
{
    if (m_conn == nullptr) return;

    m_db = m_conn->db();
    m_user = m_conn->user();
    m_pass = m_conn->pass();
    m_host = m_conn->host();
    m_port = m_conn->port();
    m_options = m_conn->options();

    if (!PGSQL::IgnoreNotice) {
        m_conn->setNoticeProcessor(notice_processor, this);
    } else {
        m_conn->setNoticeProcessor<PGSQL>(notice_processor, nullptr);
    }
}

PGSQL::PGSQL(String conninfo)
    : m_conn_string(conninfo.data()), m_last_notice("") {

    m_conn = new PQ::Connection(conninfo.data());

    if (RuntimeOption::EnableStats && RuntimeOption::EnableSQLStats) {
        ServerStats::Log("sql.conn", 1);
    }

    ConnStatusType st = m_conn->status();
    if (m_conn && st == CONNECTION_OK) {
        // Load up the fixed information
        SetupInformation();
    } else if (st == CONNECTION_BAD) {
        m_conn->finish();
    }

}


PGSQL::PGSQL(PGSQLConnectionPool &connectionPool)
    : m_conn_string(connectionPool.GetConnectionString()),
      m_last_notice("")
{
    m_conn = &(connectionPool.GetConnection());
    m_connectionPool = &connectionPool;

    SetupInformation();
}



PGSQL::~PGSQL() {
    ReleaseConnection();
}

void PGSQL::sweep() {
    ReleaseConnection();
}


void PGSQL::ReleaseConnection()
{
    if (m_conn == nullptr) return;

    if (!IsConnectionPooled())
    {
        m_conn->finish();
    }
    else
    {
        m_connectionPool->Release(*m_conn);
        m_connectionPool = nullptr;
        m_conn = nullptr;
    }

}

PGSQLResult *PGSQLResult::Get(const Variant& result) {
    if (result.isNull()) {
        return nullptr;
    }

    auto *res = result.toResource().getTyped<PGSQLResult>(true, true);
    return res;
}

PGSQLResult::PGSQLResult(PGSQL * conn, PQ::Result res)
    : m_current_row(0), m_res(std::move(res)),
      m_num_fields(-1), m_num_rows(-1), m_conn(conn) {
    m_conn->incRefCount();
}

void PGSQLResult::close() {
    m_res.clear();
}

PGSQLResult::~PGSQLResult() {
    m_conn->decRefCount();
    close();
}

void PGSQLResult::sweep() {
    close();
}

int PGSQLResult::getFieldNumber(const Variant& field) {
    int n;
    if (field.isNumeric(true)) {
        n = field.toInt32();
    } else if (field.isString()){
        n = m_res.fieldNumber(field.asCStrRef().data());
    } else {
        n = -1;
    }

    return n;
}

int PGSQLResult::getNumFields() {
    if (m_num_fields == -1) {
        m_num_fields = m_res.numFields();
    }
    return m_num_fields;
}

int PGSQLResult::getNumRows() {
    if (m_num_rows == -1) {
        m_num_rows = m_res.numTuples();
    }
    return m_num_rows;
}

bool PGSQLResult::convertFieldRow(const Variant& row, const Variant& field,
        int *out_row, int *out_field, const char *fn_name) {

    Variant actual_field;
    int actual_row;

    assert(out_row && out_field && "Output parameters cannot be null");

    if (fn_name == nullptr) {
        fn_name = "__internal_pgsql_func";
    }

    if (field.isInitialized()) {
        actual_row = row.toInt64();
        actual_field = field;
    } else {
        actual_row = m_current_row;
        actual_field = row;
    }

    int field_number = getFieldNumber(actual_field);

    if (field_number < 0 || field_number >= getNumFields()) {
        if (actual_field.isString()) {
            raise_warning("%s(): Unknown column name \"%s\"",
                    fn_name, actual_field.asCStrRef().data());
        } else {
            raise_warning("%s(): Column offset `%d` out of range", fn_name, field_number);
        }
        return false;
    }

    if (actual_row < 0 || actual_row >= getNumRows()) {
        raise_warning("%s(): Row `%d` out of range", fn_name, actual_row);
        return false;
    }

    *out_row = actual_row;
    *out_field = field_number;

    return true;
}

Variant PGSQLResult::fieldIsNull(const Variant& row, const Variant& field, const char *fn_name) {
    int r, f;
    if (convertFieldRow(row, field, &r, &f, fn_name)) {
        return m_res.fieldIsNull(r, f) ? 1 : 0;
    }

    return false;
}

Variant PGSQLResult::getFieldVal(const Variant& row, const Variant& field, const char *fn_name) {
    int r, f;
    if (convertFieldRow(row, field, &r, &f, fn_name)) {
        return getFieldVal(r, f, fn_name);
    }

    return false;
}

String PGSQLResult::getFieldVal(int row, int field, const char *fn_name) {
    if (m_res.fieldIsNull(row, field)) {
        return null_string;
    } else {
        char * value = m_res.getValue(row, field);
        int length = m_res.getLength(row, field);

        return String(value, length, CopyString);
    }
}


//////////////////////////////////////////////////////////////////////////////////

PGSQLConnectionPool::PGSQLConnectionPool(std::string connectionString, int maximumConnections)
    :m_maximumConnections(maximumConnections),
     m_connectionString(connectionString),
     m_availableConnections(),
     m_connections()
{

}


PGSQLConnectionPool::~PGSQLConnectionPool()
{
    CloseAllConnections();
}


PQ::Connection& PGSQLConnectionPool::GetConnection()
{
    Lock lock(m_lock);

    // 1) m_availableConnections
    // 2) newconn, max 1

    m_requestedConnections++;

    while (!m_availableConnections.empty())
    {
        PQ::Connection* pconn = m_availableConnections.front();
        PQ::Connection& conn = *pconn;
        m_availableConnections.pop();

        ConnStatusType st = conn.status();
        if (conn && st == CONNECTION_OK)
        {
            return conn;
        }
        else if (st == CONNECTION_BAD)
        {
            SweepConnection(conn);

            conn.finish();
        };
    }

    if (RuntimeOption::EnableStats && RuntimeOption::EnableSQLStats) {
        ServerStats::Log("sql.conn", 1);
    }

    int maxConnections = MaximumConnections();
    int connections = m_connections.size();

    if (maxConnections > 0 && connections < maxConnections)
        raise_error("The connection pool is full, cannot open new connection.");

    PQ::Connection& conn = *(new PQ::Connection(GetConnectionString()));

    ConnStatusType st = conn.status();
    if (st == CONNECTION_OK)
    {
        m_openedConnections++;
        m_connections.push_back(&conn);
    }
    else if (st == CONNECTION_BAD)
    {
        m_errors++;

        conn.finish();

        raise_error("Getting connection from pool failed.");
    }

    if (m_cleanedConnectionString == "")
    {
        m_cleanedConnectionString.append("host=");
        m_cleanedConnectionString.append(conn.host());
        m_cleanedConnectionString.append(" port=");
        m_cleanedConnectionString.append(conn.port());
        m_cleanedConnectionString.append(" user=");
        m_cleanedConnectionString.append(conn.user());
        m_cleanedConnectionString.append(" dbname=");
        m_cleanedConnectionString.append(conn.db());
    }

    return conn;
}

void PGSQLConnectionPool::SweepConnection(PQ::Connection& connection)
{
    auto p = std::find(m_connections.begin(), m_connections.end(), &connection);

    if (p != m_connections.end())
        m_connections.erase(p);

    m_sweepedConnections++;
}

void PGSQLConnectionPool::Release(PQ::Connection& connection)
{
    Lock lock(m_lock);

    m_releasedConnections++;

    ConnStatusType st = connection.status();
    if (connection && st == CONNECTION_OK) {

        m_availableConnections.push(&connection);

    } else if (st == CONNECTION_BAD) {

        connection.finish();
        SweepConnection(connection);


    }


}

void PGSQLConnectionPool::CloseAllConnections()
{
    Lock lock(m_lock);

    while (!m_availableConnections.empty())
    {
        PQ::Connection* pconn = m_availableConnections.front();
        pconn->finish();

        m_availableConnections.pop();
    }

    for (PQ::Connection* conn : m_connections)
        conn->finish();

    m_connections.clear();
}


void PGSQLConnectionPool::CloseFreeConnections()
{
    Lock lock(m_lock);

    while (!m_availableConnections.empty())
    {
        PQ::Connection* pconn = m_availableConnections.front();
        pconn->finish();

        m_availableConnections.pop();
    }
}


PGSQLConnectionPoolContainer::PGSQLConnectionPoolContainer()
    :m_pools() {
}


PGSQLConnectionPoolContainer::~PGSQLConnectionPoolContainer() {
    for (auto & any : m_pools) {
        PGSQLConnectionPool* pool = any.second;
        pool->CloseAllConnections();
    }
}



PGSQLConnectionPool& PGSQLConnectionPoolContainer::GetPool(const std::string connString)
{
    Lock lock(m_lock);

    auto pool = m_pools[connString];

    if (pool == nullptr)
    {
        pool = new PGSQLConnectionPool(connString);

        m_pools[connString] = pool;
    }

    return *pool;
}



std::vector<PGSQLConnectionPool*>& PGSQLConnectionPoolContainer::GetPools()
{
    Lock lock(m_lock);

    std::vector<PGSQLConnectionPool*>* v = new std::vector<PGSQLConnectionPool*>();

    for (auto it : m_pools)
    {
        v->push_back(it.second);
    }

    return *v;
}


//////////////////////////////////////////////////////////////////////////////////


// Simple RAII helper to convert an array to a
// list of C strings to pass to pgsql functions. Needs
// to be like this because string conversion may-or-may
// not allocate and therefore needs to ensure that the
// underlying data lasts long enough.
struct CStringArray {
    std::vector<String> m_strings;
    std::vector<const char *> m_c_strs;

public:
    CStringArray(const Array& arr) {
        int size = arr.size();

        m_strings.reserve(size);
        m_c_strs.reserve(size);

        for (ArrayIter iter(arr); iter; ++iter) {
            const Variant &param = iter.secondRef();
            if (param.isNull()) {
                m_strings.push_back(null_string);
                m_c_strs.push_back(nullptr);
            } else {
                m_strings.push_back(param.toString());
                m_c_strs.push_back(m_strings.back().data());
            }
        }
    }

    const char * const *data() {
        return m_c_strs.data();
    }

};

//////////////////// Connection functions /////////////////////////

static Variant HHVM_FUNCTION(pg_connect, const String& connection_string, int connect_type /* = 0 */) {
    PGSQL * pgsql = nullptr;

    pgsql = NEWRES(PGSQL)(connection_string);

    if (!pgsql->get()) {
        delete pgsql;
        FAIL_RETURN;
    }
    return Resource(pgsql);
}


static Variant HHVM_FUNCTION(pg_pconnect, const String& connection_string, int connect_type /* = 0 */) {
    PGSQL * pgsql = nullptr;

    PGSQLConnectionPool& pool = s_connectionPoolContainer.GetPool(connection_string.toCppString());

    pgsql = NEWRES(PGSQL)(pool);

    if (!pgsql->get()) {
        delete pgsql;
        FAIL_RETURN;
    }
    return Resource(pgsql);
}

static bool HHVM_FUNCTION(pg_close, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);
    if (pgsql) {

        pgsql->ReleaseConnection();

        return true;
    } else {
        return false;
    }
}

static bool HHVM_FUNCTION(pg_ping, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);

    if (!pgsql->get()) {
        return false;
    }

    PGPing response = PQping(pgsql->m_conn_string.data());

    if (response == PQPING_OK) {
        if (pgsql->get().status() == CONNECTION_BAD) {
            pgsql->get().reset();
            return pgsql->get().status() != CONNECTION_BAD;
        } else {
            return true;
        }
    }

    return false;
}

static bool HHVM_FUNCTION(pg_connection_reset, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);

    if (!pgsql->get()) {
        return false;
    }

    pgsql->get().reset();

    return pgsql->get().status() != CONNECTION_BAD;
}


//////////////////// Connection Pool functions /////////////////////////


const StaticString
    s_connection_string("connection_string"),
    s_sweeped_connections("sweeped_connections"),
    s_opened_connections("opened_connections"),
    s_requested_connections("requested_connections"),
    s_released_connections("released_connections"),
    s_errors("errors"),
    s_total_connections("total_connections"),
    s_free_connections("free_connections");

static Variant HHVM_FUNCTION(pg_connection_pool_stat) {



    auto pools = s_connectionPoolContainer.GetPools();

    Array arr;

    int i = 0;

    for (auto pool : pools)
    {
        Array poolArr;

        String poolName(pool->GetCleanedConnectionString().c_str(), CopyString);

        poolArr.set(s_connection_string, poolName);
        poolArr.set(s_sweeped_connections, pool->SweepedConnections());
        poolArr.set(s_opened_connections, pool->OpenedConnections());
        poolArr.set(s_requested_connections, pool->RequestedConnections());
        poolArr.set(s_released_connections, pool->ReleasedConnections());
        poolArr.set(s_errors, pool->Errors());
        poolArr.set(s_total_connections, pool->TotalConnectionsCount());
        poolArr.set(s_free_connections, pool->FreeConnectionsCount());

        arr.set(i, poolArr);
        i++;
    }

    return arr;
}



static void HHVM_FUNCTION(pg_connection_pool_sweep_free) {

    auto pools = s_connectionPoolContainer.GetPools();

    for (auto pool : pools)
    {
        pool->CloseFreeConnections();
    }

}


///////////// Interrogation Functions ////////////////////

static int64_t HHVM_FUNCTION(pg_connection_status, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);
    if (pgsql == nullptr) return CONNECTION_BAD;
    return (int64_t)pgsql->get().status();
}

static bool HHVM_FUNCTION(pg_connection_busy, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);
    if (pgsql == nullptr) {
        return false;
    }

    auto blocking = pgsql->asNonBlocking();

    pgsql->get().consumeInput();
    return pgsql->get().isBusy();
}

static Variant HHVM_FUNCTION(pg_dbname, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);

    if (pgsql == nullptr) {
        FAIL_RETURN;
    }

    return pgsql->m_db;
}

static Variant HHVM_FUNCTION(pg_host, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);

    if (pgsql == nullptr) {
        FAIL_RETURN;
    }

    return pgsql->m_host;
}

static Variant HHVM_FUNCTION(pg_port, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);

    if (pgsql == nullptr) {
        FAIL_RETURN;
    }

    String ret = pgsql->m_port;
    if (ret.isNumeric()) {
        return ret.toInt32();
    } else {
        return ret;
    }
}

static Variant HHVM_FUNCTION(pg_options, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);

    if (pgsql == nullptr) {
        FAIL_RETURN;
    }

    return pgsql->m_options;
}

static Variant HHVM_FUNCTION(pg_parameter_status, const Resource& connection, const String& param_name) {
    PGSQL * pgsql = PGSQL::Get(connection);

    if (pgsql == nullptr) {
        return false;
    }

    String ret(pgsql->get().parameterStatus(param_name.data()), CopyString);

    return ret;
}

static Variant HHVM_FUNCTION(pg_client_encoding, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);

    if (pgsql == nullptr) {
        FAIL_RETURN;
    }

    String ret(pgsql->get().clientEncoding(), CopyString);

    return ret;
}

static int64_t HHVM_FUNCTION(pg_transaction_status, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);

    if (pgsql == nullptr) {
        return PQTRANS_UNKNOWN;
    }

    return (int64_t)pgsql->get().transactionStatus();
}

static Variant HHVM_FUNCTION(pg_last_error, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);
    if (pgsql == nullptr) {
        FAIL_RETURN;
    }

    String ret(pgsql->get().errorMessage(), CopyString);

    return f_trim(ret);
}

static Variant HHVM_FUNCTION(pg_last_notice, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);
    if (pgsql == nullptr) {
        FAIL_RETURN;
    }

    return pgsql->m_last_notice;
}

static Variant HHVM_FUNCTION(pg_version, const Resource& connection) {
    static StaticString client_key("client");
    static StaticString protocol_key("protocol");
    static StaticString server_key("server");

    PGSQL * pgsql = PGSQL::Get(connection);
    if (pgsql == nullptr) {
        FAIL_RETURN;
    }

    Array ret;

    int proto_ver = pgsql->get().protocolVersion();
    if (proto_ver) {
        ret.set(protocol_key, String(proto_ver) + ".0");
    }

    int server_ver = pgsql->get().serverVersion();
    if (server_ver) {
        int revision = server_ver % 100;
        int minor = (server_ver / 100) % 100;
        int major = server_ver / 10000;

        ret.set(server_key, String(major) + "." + String(minor) + "." + String(revision));
    }

    int client_ver = PQlibVersion();
    if (client_ver) {
        int revision = client_ver % 100;
        int minor = (client_ver / 100) % 100;
        int major = client_ver / 10000;

        ret.set(client_key, String(major) + "." + String(minor) + "." + String(revision));
    }

    return ret;
}

static int64_t HHVM_FUNCTION(pg_get_pid, const Resource& connection) {
    PGSQL * pgsql = PGSQL::Get(connection);
    if (pgsql == nullptr) {
        return -1;
    }

    return (int64_t)pgsql->get().backendPID();
}

//////////////// Escaping Functions ///////////////////////////

static String HHVM_FUNCTION(pg_escape_bytea, const Resource& connection, const String& data) {
    PGSQL * pgsql = PGSQL::Get(connection);
    if (pgsql == nullptr) {
        return null_string;
    }

    std::string escaped = pgsql->get().escapeByteA(data.data(), data.size());

    if (escaped.empty()) {
        raise_warning("pg_escape_bytea(): %s", pgsql->get().errorMessage());
        return null_string;
    }

    String ret(escaped);

    return ret;
}

static String HHVM_FUNCTION(pg_escape_identifier, const Resource& connection, const String& data) {
    PGSQL * pgsql = PGSQL::Get(connection);
    if (pgsql == nullptr) {
        return null_string;
    }

    std::string escaped = pgsql->get().escapeIdentifier(data.data(), data.size());

    if (escaped.empty()) {
        raise_warning("pg_escape_identifier(): %s", pgsql->get().errorMessage());
        return null_string;
    }

    String ret(escaped);

    return ret;
}

static String HHVM_FUNCTION(pg_escape_literal, const Resource& connection, const String& data) {
    PGSQL * pgsql = PGSQL::Get(connection);
    if (pgsql == nullptr) {
        return null_string;
    }

    std::string escaped = pgsql->get().escapeLiteral(data.data(), data.size());

    if (escaped.empty()) {
        raise_warning("pg_escape_literal(): %s", pgsql->get().errorMessage());
        return null_string;
    }

    String ret(escaped);

    return ret;
}

static String HHVM_FUNCTION(pg_escape_string, const Resource& connection, const String& data) {
    PGSQL * pgsql = PGSQL::Get(connection);
    if (pgsql == nullptr) {
        return null_string;
    }

    String ret((data.size()*2)+1, ReserveString); // Reserve enough space as defined by PQescapeStringConn

    int error = 0;
    size_t size = pgsql->get().escapeString(
            ret.get()->mutableData(), data.data(), data.size(),
            &error);

    if (error) {
        return null_string;
    }

    ret.shrink(size); // Shrink to the returned size, `shrink` may re-alloc and free up space

    return ret;
}

static String HHVM_FUNCTION(pg_unescape_bytea, const String& data) {
    size_t to_len = 0;
    char * unesc = (char *)PQunescapeBytea((unsigned char *)data.data(), &to_len);
    String ret = String(unesc, to_len, CopyString);
    PQfreemem(unesc);
    return ret;
}

///////////// Command Execution / Querying /////////////////////////////

static int64_t HHVM_FUNCTION(pg_affected_rows, const Resource& result) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) return 0;

    return (int64_t)res->get().cmdTuples();
}

static Variant HHVM_FUNCTION(pg_result_status, const Resource& result, int64_t type /* = PGSQL_STATUS_LONG */) {
    PGSQLResult *res = PGSQLResult::Get(result);

    if (type == PGSQL_STATUS_LONG) {
        if (res == nullptr) return 0;

        return (int64_t)res->get().status();
    } else {
        if (res == nullptr) return null_string;

        String ret(res->get().cmdStatus(), CopyString);
        return ret;
    }
}

static bool HHVM_FUNCTION(pg_free_result, const Resource& result) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res) {
        res->close();
        return true;
    } else {
        return false;
    }
}

static bool _handle_query_result(const char *fn_name, PQ::Connection &conn, PQ::Result &result) {
    if (!result) {
        const char * err = conn.errorMessage();
        raise_warning("%s(): Query failed: %s", fn_name, err);
        return true;
    } else {
        int st = result.status();
        switch (st) {
            default:
                break;
            case PGRES_EMPTY_QUERY:
            case PGRES_BAD_RESPONSE:
            case PGRES_NONFATAL_ERROR:
            case PGRES_FATAL_ERROR:
                const char * msg = result.errorMessage();
                raise_warning("%s(): Query failed: %s", fn_name, msg);
                return true;
        }
        return false;
    }

}

static Variant HHVM_FUNCTION(pg_query, const Resource& connection, const String& query) {
    PGSQL *conn = PGSQL::Get(connection);
    if (conn == nullptr) {
        FAIL_RETURN;
    }

    PQ::Result res = conn->get().exec(query.data());

    if (_handle_query_result("pg_query", conn->get(), res))
        FAIL_RETURN;

    PGSQLResult *pgresult = NEWRES(PGSQLResult)(conn, std::move(res));

    return Resource(pgresult);
}

static Variant HHVM_FUNCTION(pg_query_params, const Resource& connection, const String& query, const Array& params) {
    PGSQL *conn = PGSQL::Get(connection);
    if (conn == nullptr) {
        FAIL_RETURN;
    }

    CStringArray str_array(params);

    PQ::Result res = conn->get().exec(query.data(), params.size(), str_array.data());

    if (_handle_query_result("pg_query_params", conn->get(), res))
        FAIL_RETURN;

    PGSQLResult *pgresult = NEWRES(PGSQLResult)(conn, std::move(res));

    return Resource(pgresult);
}

static Variant HHVM_FUNCTION(pg_prepare, const Resource& connection, const String& stmtname, const String& query) {
    PGSQL *conn = PGSQL::Get(connection);
    if (conn == nullptr) {
        FAIL_RETURN;
    }

    PQ::Result res = conn->get().prepare(stmtname.data(), query.data(), 0);

    if (_handle_query_result("pg_prepare", conn->get(), res))
        FAIL_RETURN;

    PGSQLResult *pgres = NEWRES(PGSQLResult)(conn, std::move(res));

    return Resource(pgres);
}

static Variant HHVM_FUNCTION(pg_execute, const Resource& connection, const String& stmtname, const Array& params) {
    PGSQL *conn = PGSQL::Get(connection);
    if (conn == nullptr) {
        FAIL_RETURN;
    }

    CStringArray str_array(params);

    PQ::Result res = conn->get().execPrepared(stmtname.data(), params.size(), str_array.data());
    if (_handle_query_result("pg_execute", conn->get(), res)) {
        FAIL_RETURN;
    }

    PGSQLResult *pgres = NEWRES(PGSQLResult)(conn, std::move(res));

    return Resource(pgres);
}

static bool HHVM_FUNCTION(pg_send_query, const Resource& connection, const String& query) {
    PGSQL *conn = PGSQL::Get(connection);
    if (conn == nullptr) {
        return false;
    }

    auto nb = conn->asNonBlocking();

    bool empty = true;
    PQ::Result res = conn->get().result();
    while (res) {
        res.clear();
        empty = false;
        res = conn->get().result();
    }
    if (!empty) {
        raise_notice("There are results on this connection."
                     " Call pg_get_result() until it returns FALSE");
    }

    if (!conn->get().sendQuery(query.data())) {
        // TODO: Do persistent auto-reconnect
        return false;
    }

    int ret;
    while ((ret = conn->get().flush())) {
        if (ret == -1) {
            raise_notice("Could not empty PostgreSQL send buffer");
            break;
        }
        usleep(5000);
    }

    return true;
}

static Variant HHVM_FUNCTION(pg_get_result, const Resource& connection) {
    PGSQL *conn = PGSQL::Get(connection);
    if (conn == nullptr) {
        FAIL_RETURN;
    }

    PQ::Result res = conn->get().result();

    if (!res) {
        FAIL_RETURN;
    }

    PGSQLResult *pgresult = NEWRES(PGSQLResult)(conn, std::move(res));

    return Resource(pgresult);
}

static bool HHVM_FUNCTION(pg_send_query_params, const Resource& connection, const String& query, const Array& params) {
    PGSQL *conn = PGSQL::Get(connection);
    if (conn == nullptr) {
        return false;
    }

    auto nb = conn->asNonBlocking();

    bool empty = true;
    PQ::Result res = conn->get().result();
    while (res) {
        res.clear();
        empty = false;
        res = conn->get().result();
    }
    if (!empty) {
        raise_notice("There are results on this connection."
                     " Call pg_get_result() until it returns FALSE");
    }

    CStringArray str_array(params);

    if (!conn->get().sendQuery(query.data(), params.size(), str_array.data())) {
        return false;
    }

    int ret;
    while ((ret = conn->get().flush())) {
        if (ret == -1) {
            raise_notice("Could not empty PostgreSQL send buffer");
            break;
        }
        usleep(5000);
    }

    return true;
}

static bool HHVM_FUNCTION(pg_send_prepare, const Resource& connection, const String& stmtname, const String& query) {
    PGSQL *conn = PGSQL::Get(connection);
    if (conn == nullptr) {
        return false;
    }

    return conn->get().sendPrepare(stmtname.data(), query.data(), 0);
}

static bool HHVM_FUNCTION(pg_send_execute, const Resource& connection, const String& stmtname, const Array& params) {
    PGSQL *conn = PGSQL::Get(connection);
    if (conn == nullptr) {
        return false;
    }

    CStringArray str_array(params);

    return conn->get().sendQueryPrepared(stmtname.data(),
            params.size(), str_array.data());
}

static bool HHVM_FUNCTION(pg_cancel_query, const Resource& connection) {
    PGSQL *conn = PGSQL::Get(connection);
    if (conn == nullptr) {
        return false;
    }

    auto nb = conn->asNonBlocking();

    bool ret = conn->get().cancelRequest();

    PQ::Result res = conn->get().result();
    while(res) {
        res.clear();
        res = conn->get().result();
    }

    return ret;
}

////////////////////////

static Variant HHVM_FUNCTION(pg_fetch_all_columns, const Resource& result, int64_t column /* = 0 */) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        FAIL_RETURN;
    }

    if (column < 0 || column >= res->getNumFields()) {
        raise_warning("pg_fetch_all_columns(): Column offset `%d` out of range", (int)column);
        FAIL_RETURN;
    }

    int num_rows = res->getNumRows();

    Array arr;
    for (int i = 0; i < num_rows; i++) {
        Variant field = res->getFieldVal(i, column);
        arr.set(i, field);
    }

    return arr;
}

static Variant HHVM_FUNCTION(pg_fetch_array, const Resource& result, const Variant& row /* = null_variant */, int64_t result_type /* = PGSQL_BOTH */) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        FAIL_RETURN;
    }

    int r;
    if (row.isNull()) {
        r = res->m_current_row;
        if (r >= res->getNumRows()) {
            FAIL_RETURN;
        }
        res->m_current_row++;
    } else {
        r = row.toInt32();
    }

    if (r < 0 || r >= res->getNumRows()) {
        raise_warning("Row `%d` out of range", r);
        FAIL_RETURN;
    }

    Array arr;

    for (int i = 0; i < res->getNumFields(); i++) {
        Variant field = res->getFieldVal(r, i);
        if (result_type & PGSQL_NUM) {
            arr.set(i, field);
        }
        if (result_type & PGSQL_ASSOC) {
            char * name = res->get().fieldName(i);
            String fn(name, CopyString);
            arr.set(fn, field);
        }
    }

    return arr;
}

static Variant HHVM_FUNCTION(pg_fetch_assoc, const Resource& result, const Variant& row /* = null_variant */) {
    return f_pg_fetch_array(result, row, PGSQL_ASSOC);
}

static Variant HHVM_FUNCTION(pg_fetch_all, const Resource& result) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        FAIL_RETURN;
    }

    int num_rows = res->getNumRows();
    if (num_rows == 0) {
        FAIL_RETURN;
    }

    Array rows;
    for (int i = 0; i < num_rows; i++) {
        Variant row = f_pg_fetch_assoc(result, i);
        rows.set(i, row);
    }

    return rows;
}

static Variant HHVM_FUNCTION(pg_fetch_result, const Resource& result, const Variant& row /* = null_variant */, const Variant& field /* = null_variant */) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        FAIL_RETURN;
    }

    return res->getFieldVal(row, field, "pg_fetch_result");
}

static Variant HHVM_FUNCTION(pg_fetch_row, const Resource& result, const Variant& row /* = null_variant */) {
    return f_pg_fetch_array(result, row, PGSQL_NUM);
}

///////////////////// Field information //////////////////////////

static Variant HHVM_FUNCTION(pg_field_is_null, const Resource& result, const Variant& row, const Variant& field /* = null_variant */) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        FAIL_RETURN;
    }

    return res->fieldIsNull(row, field, "pg_field_is_null");
}

static Variant HHVM_FUNCTION(pg_field_name, const Resource& result, int64_t field_number) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        FAIL_RETURN;
    }

    if (field_number < 0 || field_number >= res->getNumFields()) {
        raise_warning("pg_field_name(): Column offset `%d` out of range", (int)field_number);
        FAIL_RETURN;
    }

    char * name = res->get().fieldName((int)field_number);
    if (name == nullptr) {
        raise_warning("pg_field_name(): %s", res->get().errorMessage());
        FAIL_RETURN;
    } else {
        return String(name, CopyString);
    }
}

static int64_t HHVM_FUNCTION(pg_field_num, const Resource& result, const String& field_name) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        return -1;
    }

    return res->get().fieldNumber(field_name.data());
}

static Variant HHVM_FUNCTION(pg_field_prtlen, const Resource& result, const Variant& row_number, const Variant& field /* = null_variant */) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        FAIL_RETURN;
    }

    int r, f;
    if (res->convertFieldRow(row_number, field, &r, &f, "pg_field_prtlen")) {
        return res->get().getLength(r, f);
    }
    FAIL_RETURN;
}

static Variant HHVM_FUNCTION(pg_field_size, const Resource& result, int64_t field_number) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        FAIL_RETURN;
    }

    if (field_number < 0 || field_number > res->getNumFields()) {
        raise_warning("pg_field_size(): Column offset `%d` out of range", (int)field_number);
        FAIL_RETURN;
    }

    return res->get().size(field_number);
}

static Variant HHVM_FUNCTION(pg_field_table, const Resource& result, int64_t field_number, bool oid_only /* = false */) {
    PGSQLResult *res = PGSQLResult::Get(result);

    if (res == nullptr) {
        FAIL_RETURN;
    }

    if (field_number < 0 || field_number > res->getNumFields()) {
        raise_warning("pg_field_table(): Column offset `%d` out of range", (int)field_number);
        FAIL_RETURN;
    }

    Oid id = res->get().table(field_number);
    if (id == InvalidOid) FAIL_RETURN;

    if (oid_only) {
        return (int64_t)id;
    } else {
        // TODO: cache the Oids somewhere
        std::ostringstream query;
        query << "SELECT relname FROM pg_class WHERE oid=" << id;

        PQ::Result name_res = res->getConn()->get().exec(query.str().c_str());
        if (!name_res)
            FAIL_RETURN;

        if (name_res.status() != PGRES_TUPLES_OK)
            FAIL_RETURN;

        char * name = name_res.getValue(0, 0);
        if (name == nullptr) {
            FAIL_RETURN;
        }

        String ret(name, CopyString);

        return ret;
    }
}

static Variant HHVM_FUNCTION(pg_field_type_oid, const Resource& result, int64_t field_number) {
    PGSQLResult *res = PGSQLResult::Get(result);

    if (res == nullptr) {
        FAIL_RETURN;
    }

    if (field_number < 0 || field_number > res->getNumFields()) {
        raise_warning("pg_field_table(): Column offset `%d` out of range", (int)field_number);
        FAIL_RETURN;
    }

    Oid id = res->get().type(field_number);
    if (id == InvalidOid) FAIL_RETURN;
    return (int64_t)id;
}

// TODO: Cache the results of this function somewhere
static Variant HHVM_FUNCTION(pg_field_type, const Resource& result, int64_t field_number) {
    PGSQLResult *res = PGSQLResult::Get(result);

    if (res == nullptr) {
        FAIL_RETURN;
    }

    if (field_number < 0 || field_number > res->getNumFields()) {
        raise_warning("pg_field_type(): Column offset `%d` out of range", (int)field_number);
        FAIL_RETURN;
    }

    Oid id = res->get().type(field_number);
    if (id == InvalidOid) FAIL_RETURN;

    // This should really get all of the rows in pg_type and build a map
    std::ostringstream query;
        query << "SELECT typname FROM pg_type WHERE oid=" << id;

    PQ::Result name_res = res->getConn()->get().exec(query.str().c_str());
    if (!name_res)
        FAIL_RETURN;

    if (name_res.status() != PGRES_TUPLES_OK)
        FAIL_RETURN;

    char * name = name_res.getValue(0, 0);
    if (name == nullptr)
        FAIL_RETURN;

    String ret(name, CopyString);

    return ret;
}

static int64_t HHVM_FUNCTION(pg_num_fields, const Resource& result) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        return -1;
    }

    return res->getNumFields();
}

static int64_t HHVM_FUNCTION(pg_num_rows, const Resource& result) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        return -1;
    }

    return res->getNumRows();
}

static Variant HHVM_FUNCTION(pg_result_error_field, const Resource& result, int64_t fieldcode) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        FAIL_RETURN;
    }

    char * msg = res->get().errorField(fieldcode);
    if (msg) {
        return f_trim(String(msg, CopyString));
    }

    FAIL_RETURN;
}

static Variant HHVM_FUNCTION(pg_result_error, const Resource& result) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        FAIL_RETURN;
    }

    const char * msg = res->get().errorMessage();
    if (msg) {
        return f_trim(String(msg, CopyString));
    }

    FAIL_RETURN;
}

static bool HHVM_FUNCTION(pg_result_seek, const Resource& result, int64_t offset) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        return false;
    }

    if (offset < 0 || offset > res->getNumRows()) {
        raise_warning("pg_result_seek(): Cannot seek to row %d", (int)offset);
        return false;
    }

    res->m_current_row = (int)offset;
    return true;
}

static Variant HHVM_FUNCTION(pg_last_oid, const Resource& result) {
    PGSQLResult *res = PGSQLResult::Get(result);
    if (res == nullptr) {
        FAIL_RETURN;
    }

    Oid oid = res->get().oidValue();

    if (oid == InvalidOid) FAIL_RETURN;
    else return String((int64_t)oid);
}
///////////////////////////////////////////////////////////////////////////////

bool PGSQL::AllowPersistent     = true;
int  PGSQL::MaxPersistent       = -1;
int  PGSQL::MaxLinks            = -1;
bool PGSQL::AutoResetPersistent = false;
bool PGSQL::IgnoreNotice        = false;
bool PGSQL::LogNotice           = false;

namespace { // Anonymous Namespace
static class pgsqlExtension : public Extension {
public:
    pgsqlExtension() : Extension("pgsql") {}

    virtual void moduleLoad(const IniSetting::Map& ini, Hdf hdf)
    {
        Hdf pgsql = hdf["PGSQL"];

        PGSQL::AllowPersistent     = Config::GetBool(ini, pgsql["AllowPersistent"], true);
        PGSQL::MaxPersistent       = Config::GetInt32(ini, pgsql["MaxPersistent"], -1);
        PGSQL::MaxLinks            = Config::GetInt32(ini, pgsql["MaxLinks"], -1);
        PGSQL::AutoResetPersistent = Config::GetBool(ini, pgsql["AutoResetPersistent"]);
        PGSQL::IgnoreNotice        = Config::GetBool(ini, pgsql["IgnoreNotice"]);
        PGSQL::LogNotice           = Config::GetBool(ini, pgsql["LogNotice"]);

    }

    virtual void moduleInit() {

        HHVM_FE(pg_affected_rows);
        HHVM_FE(pg_cancel_query);
        HHVM_FE(pg_client_encoding);
        HHVM_FE(pg_close);
        HHVM_FE(pg_connect);
        HHVM_FE(pg_pconnect);
        HHVM_FE(pg_connection_pool_stat);
        HHVM_FE(pg_connection_pool_sweep_free);
        HHVM_FE(pg_connection_busy);
        HHVM_FE(pg_connection_reset);
        HHVM_FE(pg_connection_status);
        HHVM_FE(pg_dbname);
        HHVM_FE(pg_escape_bytea);
        HHVM_FE(pg_escape_identifier);
        HHVM_FE(pg_escape_literal);
        HHVM_FE(pg_escape_string);
        HHVM_FE(pg_execute);
        HHVM_FE(pg_fetch_all_columns);
        HHVM_FE(pg_fetch_all);
        HHVM_FE(pg_fetch_array);
        HHVM_FE(pg_fetch_assoc);
        HHVM_FE(pg_fetch_result);
        HHVM_FE(pg_fetch_row);
        HHVM_FE(pg_field_is_null);
        HHVM_FE(pg_field_name);
        HHVM_FE(pg_field_num);
        HHVM_FE(pg_field_prtlen);
        HHVM_FE(pg_field_size);
        HHVM_FE(pg_field_table);
        HHVM_FE(pg_field_type_oid);
        HHVM_FE(pg_field_type);
        HHVM_FE(pg_free_result);
        HHVM_FE(pg_get_pid);
        HHVM_FE(pg_get_result);
        HHVM_FE(pg_host);
        HHVM_FE(pg_last_error);
        HHVM_FE(pg_last_notice);
        HHVM_FE(pg_last_oid);
        HHVM_FE(pg_num_fields);
        HHVM_FE(pg_num_rows);
        HHVM_FE(pg_options);
        HHVM_FE(pg_parameter_status);
        HHVM_FE(pg_ping);
        HHVM_FE(pg_port);
        HHVM_FE(pg_prepare);
        HHVM_FE(pg_query_params);
        HHVM_FE(pg_query);
        HHVM_FE(pg_result_error_field);
        HHVM_FE(pg_result_error);
        HHVM_FE(pg_result_seek);
        HHVM_FE(pg_result_status);
        HHVM_FE(pg_send_execute);
        HHVM_FE(pg_send_prepare);
        HHVM_FE(pg_send_query_params);
        HHVM_FE(pg_send_query);
        HHVM_FE(pg_transaction_status);
        HHVM_FE(pg_unescape_bytea);
        HHVM_FE(pg_version);

#define C(name, value) Native::registerConstant<KindOfInt64>(makeStaticString("PGSQL_" #name), (value))
        // Register constants

        C(ASSOC, PGSQL_ASSOC);
        C(NUM, PGSQL_NUM);
        C(BOTH, PGSQL_BOTH);

        C(CONNECT_FORCE_NEW, 1);
        C(CONNECTION_BAD, CONNECTION_BAD);
        C(CONNECTION_OK, CONNECTION_OK);
        C(CONNECTION_STARTED, CONNECTION_STARTED);
        C(CONNECTION_MADE, CONNECTION_MADE);
        C(CONNECTION_AWAITING_RESPONSE, CONNECTION_AWAITING_RESPONSE);
        C(CONNECTION_AUTH_OK, CONNECTION_AUTH_OK);
        C(CONNECTION_SETENV, CONNECTION_SETENV);
        C(CONNECTION_SSL_STARTUP, CONNECTION_SSL_STARTUP);

        C(SEEK_SET, SEEK_SET);
        C(SEEK_CUR, SEEK_CUR);
        C(SEEK_END, SEEK_END);

        C(EMPTY_QUERY, PGRES_EMPTY_QUERY);
        C(COMMAND_OK, PGRES_COMMAND_OK);
        C(TUPLES_OK, PGRES_TUPLES_OK);
        C(COPY_OUT, PGRES_COPY_OUT);
        C(COPY_IN, PGRES_COPY_IN);
        C(BAD_RESPONSE, PGRES_BAD_RESPONSE);
        C(NONFATAL_ERROR, PGRES_NONFATAL_ERROR);
        C(FATAL_ERROR, PGRES_FATAL_ERROR);

        C(TRANSACTION_IDLE, PQTRANS_IDLE);
        C(TRANSACTION_ACTIVE, PQTRANS_ACTIVE);
        C(TRANSACTION_INTRANS, PQTRANS_INTRANS);
        C(TRANSACTION_INERROR, PQTRANS_INERROR);
        C(TRANSACTION_UNKNOWN, PQTRANS_UNKNOWN);

        C(DIAG_SEVERITY, PG_DIAG_SEVERITY);
        C(DIAG_SQLSTATE, PG_DIAG_SQLSTATE);
        C(DIAG_MESSAGE_PRIMARY, PG_DIAG_MESSAGE_PRIMARY);
        C(DIAG_MESSAGE_DETAIL, PG_DIAG_MESSAGE_DETAIL);
        C(DIAG_MESSAGE_HINT, PG_DIAG_MESSAGE_HINT);
        C(DIAG_STATEMENT_POSITION, PG_DIAG_STATEMENT_POSITION);
        C(DIAG_INTERNAL_POSITION, PG_DIAG_INTERNAL_POSITION);
        C(DIAG_INTERNAL_QUERY, PG_DIAG_INTERNAL_QUERY);
        C(DIAG_CONTEXT, PG_DIAG_CONTEXT);
        C(DIAG_SOURCE_FILE, PG_DIAG_SOURCE_FILE);
        C(DIAG_SOURCE_LINE, PG_DIAG_SOURCE_LINE);
        C(DIAG_SOURCE_FUNCTION, PG_DIAG_SOURCE_FUNCTION);

        C(ERRORS_TERSE, PQERRORS_TERSE);
        C(ERRORS_DEFAULT, PQERRORS_DEFAULT);
        C(ERRORS_VERBOSE, PQERRORS_VERBOSE);

        C(STATUS_LONG, PGSQL_STATUS_LONG);
        C(STATUS_STRING, PGSQL_STATUS_STRING);

        C(CONV_IGNORE_DEFAULT, 1);
        C(CONV_FORCE_NULL, 2);
        C(CONV_IGNORE_NOT_NULL, 4);

#undef C
        loadSystemlib();
    }
} s_pgsql_extension;

}

extern "C" Extension *getModule() {
    return &s_pgsql_extension;
}

///////////////////////////////////////////////////////////////////////////////

}

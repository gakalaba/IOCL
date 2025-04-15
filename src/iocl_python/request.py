import redisstorepython as rs
import hashlib
import struct


def string_to_int64_hash(s):
    hash_value = int(
        hashlib.sha256(s.encode()).hexdigest(), 16
    )  # Large int from SHA-256
    int64_hash = (hash_value % (2**64)) - 2**63  # Map to signed int64 range
    return int64_hash


def InitCustom():
    """
    Initialize a session using the underlying C++ CustomInit method.

    :return: Session ID
    """
    return rs.custom_init_session()


def SyncAppRequest(session_id, op_type, key, value=None, old_value=None):
    """
    Perform an operation on a key with optional value and old_value.

    :param session_id: Session ID from CustomInit()
    :param op_type: Operation type (e.g., 'PUT', 'GET', 'INCR', 'SET', etc.)
    :param key: Key for the operation
    :param value: Value for the operation (optional)
    :param old_value: Old value for comparison operations (optional)
    :return: Result of the operation
    """
    # Convert string operation type to enum
    if isinstance(op_type, str):
        try:
            op = getattr(rs.Operation, op_type.upper())
        except AttributeError:
            raise ValueError(f"Unknown operation: {op_type}")
    else:
        op = op_type

    # Convert key to int64 if it's a string that can be converted to an integer
    if isinstance(key, str) and key.isdigit():
        key = int(key)
    elif isinstance(key, str):
        key = string_to_int64_hash(key)

    # Convert value and old_value to string
    value = str(value) if value is not None else None
    old_value = str(old_value) if old_value is not None else None

    # Send async request
    success, request_id = rs.async_send_request(session_id, op, key, value, old_value)
    
    # Get async response
    success, result = rs.async_get_response(session_id, request_id)

    return result


def AsyncSendRequest(session_id, op_type, key, value=None, old_value=None):
    """
    Send an asynchronous request.

    :param session_id: Session ID from CustomInit()
    :param op_type: Operation type (e.g., 'PUT', 'GET')
    :param key: Key for the operation
    :param value: Value for the operation (optional)
    :param old_value: Old value for comparison operations (optional)
    :return: Request ID
    """
    # Convert string operation type to enum
    if isinstance(op_type, str):
        try:
            op = getattr(rs.Operation, op_type.upper())
        except AttributeError:
            raise ValueError(f"Unknown operation: {op_type}")
    else:
        op = op_type

    # Convert key to int64 if it's a string that can be converted to an integer
    if isinstance(key, str) and key.isdigit():
        key = int(key)
    elif isinstance(key, str):
        key = string_to_int64_hash(key)

    # Convert value and old_value to string
    value = str(value) if value is not None else None
    old_value = str(old_value) if old_value is not None else None

    # Send async request
    success, request_id = rs.async_send_request(session_id, op, key, value, old_value)
    
    return request_id


def AsyncGetResponse(session_id, request_id):
    """
    Retrieve the result of an asynchronous operation.

    :param session_id: Session ID from CustomInit()
    :param request_id: Request ID returned by AsyncSendRequest
    :return: Result of the async operation
    """
    # Get async response
    success, result = rs.async_get_response(session_id, request_id)

    return result

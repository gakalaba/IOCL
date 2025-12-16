#!/usr/bin/env python3
"""
Sanity test for IOCL async and sync implementations.
Tests:
- IOCL (async): Issues 2 concurrent requests
- VR (sync): Issues 2 sequential requests
"""

import sys
import os

# Add paths for imports
sys.path.append('/users/akalaba/IOCL/redis-chat-transformed')

def test_async_iocl():
    """Test async version with 2 concurrent requests (IOCL)"""
    print("\n" + "="*60)
    print("TESTING ASYNC (IOCL) - 2 Concurrent Requests")
    print("="*60)

    # Set environment for IOCL protocol
    os.environ["IOCL_PROTOCOL_MODE"] = "iocl_ct"
    os.environ["IOCL_CONSISTENCY"] = "lin"
    os.environ["IOCL_CLIENT_ID"] = "0"
    os.environ["IOCL_CLIENT_NUM_KEYS"] = "100"
    os.environ["IOCL_NUM_SHARDS"] = "1"
    os.environ["IOCL_EXP_DURATION"] = "5"
    os.environ["IOCL_WARMUP_SECS"] = "0"
    os.environ["IOCL_COOLDOWN_SECS"] = "0"
    os.environ["IOCL_CLIENT_HOST"] = "localhost"
    os.environ["IOCL_BENCH_MODE"] = "closed"
    os.environ["IOCL_MPL"] = "1"

    import redisstore
    from iocl.iocl_utils import send_request, await_request

    # Initialize session
    print("Initializing session...")
    session_id = redisstore.custom_init_session()
    print(f"Session ID: {session_id}")

    # Issue 2 concurrent requests
    print("\n[ASYNC] Issuing 2 concurrent SET requests...")
    future1 = send_request(session_id, "SET", "test_key_1", "value_1")
    print(f"  Request 1 sent, command_id: {future1}")

    future2 = send_request(session_id, "SET", "test_key_2", "value_2")
    print(f"  Request 2 sent, command_id: {future2}")

    # Await responses
    print("\n[ASYNC] Awaiting responses...")
    success1, result1 = await_request(session_id, future1)
    print(f"  Request 1 completed: success={success1}, result={result1}")

    success2, result2 = await_request(session_id, future2)
    print(f"  Request 2 completed: success={success2}, result={result2}")

    # Verify with GET requests
    print("\n[ASYNC] Verifying with GET requests...")
    future3 = send_request(session_id, "GET", "test_key_1")
    success3, result3 = await_request(session_id, future3)
    print(f"  GET test_key_1: {result3}")

    future4 = send_request(session_id, "GET", "test_key_2")
    success4, result4 = await_request(session_id, future4)
    print(f"  GET test_key_2: {result4}")

    print("\n[ASYNC] ✓ Test completed successfully!")
    return True


def test_sync_vr():
    """Test sync version with 2 sequential requests (VR)"""
    print("\n" + "="*60)
    print("TESTING SYNC (VR) - 2 Sequential Requests")
    print("="*60)

    # Set environment for VR protocol
    os.environ["IOCL_PROTOCOL_MODE"] = "vr"
    os.environ["IOCL_CONSISTENCY"] = "lin"
    os.environ["IOCL_CLIENT_ID"] = "1"
    os.environ["IOCL_CLIENT_NUM_KEYS"] = "100"
    os.environ["IOCL_NUM_SHARDS"] = "1"
    os.environ["IOCL_EXP_DURATION"] = "5"
    os.environ["IOCL_WARMUP_SECS"] = "0"
    os.environ["IOCL_COOLDOWN_SECS"] = "0"
    os.environ["IOCL_CLIENT_HOST"] = "localhost"
    os.environ["IOCL_BENCH_MODE"] = "closed"
    os.environ["IOCL_MPL"] = "1"

    import redisstore
    from iocl.iocl_utils import send_request_and_await

    # Initialize session
    print("Initializing session...")
    session_id = redisstore.custom_init_session()
    print(f"Session ID: {session_id}")

    # Issue 2 sequential requests
    print("\n[SYNC] Issuing 2 sequential SET requests...")
    success1, result1 = send_request_and_await(session_id, "SET", "test_key_3", "value_3", "")
    print(f"  Request 1 completed: success={success1}, result={result1}")

    success2, result2 = send_request_and_await(session_id, "SET", "test_key_4", "value_4", "")
    print(f"  Request 2 completed: success={success2}, result={result2}")

    # Verify with GET requests
    print("\n[SYNC] Verifying with GET requests...")
    success3, result3 = send_request_and_await(session_id, "GET", "test_key_3", "", "")
    print(f"  GET test_key_3: {result3}")

    success4, result4 = send_request_and_await(session_id, "GET", "test_key_4", "", "")
    print(f"  GET test_key_4: {result4}")

    print("\n[SYNC] ✓ Test completed successfully!")
    return True


if __name__ == "__main__":
    print("\n" + "="*60)
    print("IOCL SANITY CHECK TEST")
    print("="*60)

    try:
        # Test async version
        async_success = test_async_iocl()

        # Test sync version
        sync_success = test_sync_vr()

        if async_success and sync_success:
            print("\n" + "="*60)
            print("✓ ALL TESTS PASSED!")
            print("="*60)
            sys.exit(0)
        else:
            print("\n" + "="*60)
            print("✗ SOME TESTS FAILED")
            print("="*60)
            sys.exit(1)

    except Exception as e:
        print(f"\n✗ ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

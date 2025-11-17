	4. performance tests:
		1. [x] run fanout 1, issue_concurrent false (EXACT SAME AS Step1 TEST)
		2. [x] run fanout 2, 4, 8, issue_concurrent true
			1. f1
				1. VR = 1.08ms
				2. IOCL = 1.2ms
			2. f2
				1. VR=2.18ms
				2. IOCL = 2ms
			3. f4
				1. VR=4.5
				2. IOCL = 2.7ms
			4. f8
				1. VR=8.97
				2. IOCL = 3.9ms
		3. [x] run fanout 2, 4, 8 on Step1 and COMPARE THEM
			1. THERE IS A BUG --- the VR numbers aren't sequential!! they're parallel -- so really it's an oracle
			2. f1
				1. IOCL = ??? don't have client=1
			3. f2
				1. IOCL = ??? don't have client=1
			4. f4
				1. ORACLE=1.7725980000000001
				2. IOCL = 2.6663485ms
			5. f8
				1. ORACLE=2.497398ms
				2. IOCL = 3.673162ms

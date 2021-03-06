# Copyright 2020 Vectorized, Inc.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.md
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0

import subprocess


class CompactedTopicVerifier:
    """
    Wrapper around the java based compacted topic verifier
    """
    def __init__(self, redpanda, topic='verifier_topic'):
        self._redpanda = redpanda
        self.verifier_jar = f'{self._redpanda.v_build_dir}/java-build/compacted-log-verifier/kafka-compacted-topics-verifier.jar'
        self.state_path = f'{self._redpanda.v_build_dir}/ducktape/verifier.state'
        self.topic = topic

    def produce(self,
                record_count=100,
                rf=1,
                p=1,
                segment_size=500000,
                key_sz=20,
                payload_sz=128,
                p_props='acks=-1',
                key_cardinality=100):
        cmd = ("produce "
               "--num-records {record_count} "
               "--replication-factor {rf} "
               "--partitions {p} "
               "--segment-size {seg_sz} "
               "--key-size {key_sz} "
               "--payload-size {payload_sz} "
               "--producer-props {p_props} "
               "--key-cardinality {k_card} ").format(record_count=record_count,
                                                     rf=rf,
                                                     p=p,
                                                     seg_sz=segment_size,
                                                     key_sz=key_sz,
                                                     payload_sz=payload_sz,
                                                     p_props=p_props,
                                                     k_card=key_cardinality)
        return self._cmd(cmd)

    def verify(self):
        return self._cmd('consume')

    def _cmd(self, cmd_str):
        self._redpanda.logger.debug("starting compacted topic verifier")
        try:
            cmd = ("{java} -jar {verifier_jar} --broker {brokers} "
                   "--topic {topic} "
                   "--state-file {state_path} "
                   "{cmd}").format(java="java",
                                   verifier_jar=self.verifier_jar,
                                   brokers=self._redpanda.brokers(),
                                   topic=self.topic,
                                   state_path=self.state_path,
                                   cmd=cmd_str)

            return subprocess.check_output(["/bin/sh", "-c", cmd],
                                           stderr=subprocess.STDOUT)

        except subprocess.CalledProcessError as e:
            self._redpanda.logger.error("Error (%d) executing verifier:\n %s",
                                        e.returncode, e.output)

/*
 *  Licensed to the Apache Software Foundation (ASF) under one or more
 *  contributor license agreements.  See the NOTICE file distributed with
 *  this work for additional information regarding copyright ownership.
 *  The ASF licenses this file to You under the Apache License, Version 2.0
 *  (the "License"); you may not use this file except in compliance with
 *  the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
package org.apache.commons.daemon.maven;

import java.rmi.registry.LocateRegistry;
import java.rmi.registry.Registry;
import org.apache.maven.plugin.MojoExecutionException;
import org.apache.maven.plugins.annotations.Mojo;

@Mojo (
    name = "stop",
    threadSafe = true
)
public final class StopDaemonMojo extends AbstractDaemonMojo {

    @Override
    public void execute() throws MojoExecutionException {
        try {
            final Registry registry = LocateRegistry.getRegistry(port);
            final DaemonRemote instance = (DaemonRemote) registry.lookup(RMI_PATH);
            instance.stop();
            instance.destroy();
            registry.unbind(RMI_PATH);
        } catch (Exception ex) {
            throw new MojoExecutionException(ex.getMessage(), ex);
        }
    }
}

/* This file is part of VoltDB.
 * Copyright (C) 2008-2019 VoltDB Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */

package org.voltdb;

import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.ByteBuffer;
import java.util.Vector;

import org.hsqldb_voltpatches.FunctionForVoltDB;
import org.voltcore.logging.VoltLogger;
import org.voltdb.catalog.CatalogMap;
import org.voltdb.catalog.Function;
import org.voltdb.utils.SerializationHelper;
import org.voltdb.utils.JavaBuiltInFunctions;
import org.voltdb.compiler.statements.CreateFunction;
import org.voltdb.UserDefinedFunctionRunner;

import com.google_voltpatches.common.collect.ImmutableMap;

/**
 * This class maintains the necessary information for each UDAF including the class instance
 * and the method ID for the UDAF implementation. We run UDAFs from this runner.
 * @author russelhu
 *
 */
public class UserDefinedAggregateFunctionRunner extends UserDefinedFunctionRunner {
    final String m_functionName;
    final int m_functionId;
    final String m_className;
    Method m_startMethod;
    Method m_assembleMethod;
    Method m_combineMethod;
    Method m_endMethod;
    Class<?> m_funcClass;
    Method[] m_functionMethods;
    Vector<Object> m_functionInstances;
    final VoltType[] m_paramTypes;
    final boolean[] m_boxUpByteArray;
    final VoltType m_returnType;
    final int m_paramCount;

    public UserDefinedAggregateFunctionRunner(Function catalogFunction, Class<?> funcClass) {
        this(catalogFunction.getFunctionname(), catalogFunction.getFunctionid(),
                catalogFunction.getClassname(), funcClass);
    }

    public UserDefinedAggregateFunctionRunner(String functionName, int functionId, String className, Class<?> funcClass) {
        m_functionName = functionName;
        m_functionId = functionId;
        m_className = className;
        m_funcClass = funcClass;
        initFunctionMethods();
        m_functionInstances = new Vector<Object>();
        m_startMethod = initFunctionMethod("start");
        m_assembleMethod = initFunctionMethod("assemble");
        m_combineMethod = initFunctionMethod("combine");
        m_endMethod = initFunctionMethod("end");
        Class<?>[] paramTypeClasses = m_assembleMethod.getParameterTypes();
        m_paramCount = paramTypeClasses.length;
        m_paramTypes = new VoltType[m_paramCount];
        m_boxUpByteArray = new boolean[m_paramCount];
        for (int i = 0; i < m_paramCount; i++) {
            m_paramTypes[i] = VoltType.typeFromClass(paramTypeClasses[i]);
            m_boxUpByteArray[i] = paramTypeClasses[i] == Byte[].class;
        }
        m_returnType = VoltType.typeFromClass(m_endMethod.getReturnType());

        m_logger.debug(String.format("The user-defined function manager is defining aggregate function %s (ID = %s)",
                m_functionName, m_functionId));

        // We register the token again when initializing the user-defined function manager because
        // in a cluster setting the token may only be registered on the node where the CREATE FUNCTION DDL
        // is executed. We uses a static map in FunctionDescriptor to maintain the token list.
        FunctionForVoltDB.registerTokenForUDF(m_functionName, m_functionId, m_returnType, m_paramTypes, true);
    }

    private void initFunctionMethods() {
        try {
            Object functionInstance = m_funcClass.newInstance();
            m_functionMethods = functionInstance.getClass().getDeclaredMethods();
        }
        catch (InstantiationException | IllegalAccessException e) {
            throw new RuntimeException(String.format("Error instantiating function \"%s\"", m_className), e);
        }
    }

    private void addFunctionInstance() {
        try {
            Object tempFunctionInstance = m_funcClass.newInstance();
            m_functionInstances.add(tempFunctionInstance);
        }
        catch (InstantiationException | IllegalAccessException e) {
            throw new RuntimeException(String.format("Error instantiating function \"%s\"", m_className), e);
        }
    }

    private Method initFunctionMethod(String methodName) {
        Method temp_method = null;
        for (final Method m : m_functionMethods) {
            if (m.getName().equals(methodName)) {
                if (!Modifier.isPublic(m.getModifiers())) {
                    continue;
                }
                if (Modifier.isStatic(m.getModifiers())) {
                    continue;
                }
                // The return type of start|assemble|combine function should be void
                if (!methodName.equals("end")) {
                    if (!m.getReturnType().equals(Void.TYPE)) {
                        continue;
                    }
                    // If the start method has at least one parameter, this is not the start
                    // method we're looking for
                    if (methodName.equals("start") && m.getParameterCount() > 0) {
                        continue;
                    }
                    // We only support one parameter for the assemble method currently.
                    // If we can support more parameters in the future, we need to delete this check
                    if (methodName.equals("assemble")) {
                        // If the number of parameter is not one, this is not a correct assemble method
                        if (m.getParameterCount() != 1) {
                            continue;
                        }
                        // This assemble method has exactly one parameter
                        // However, this parameter's type is not one of the allowed types
                        if (!CreateFunction.isAllowedDataType(m.getParameterTypes()[0])) {
                            continue;
                        }
                    }
                    // The combine method can have one and only one parameter which is the
                    // same type as the current class
                    if (methodName.equals("combine") && m.getParameterCount() > 0) {
                        if (m.getParameterCount() != 1) {
                            continue;
                        }
                        else if (m.getParameterTypes()[0] != m_funcClass) {
                            continue;
                        }
                    }
                }
                // However, the return type for the end function cannot be void
                else {
                    if (m.getReturnType().equals(Void.TYPE)) {
                        continue;
                    }
                    // If the end method has at least one parameter, this is not the end
                    // method we're looking for
                    if (m.getParameterCount() > 0 || !CreateFunction.isAllowedDataType(m.getReturnType())) {
                        continue;
                    }
                }
                temp_method = m;
                break;
            }
        }
        if (temp_method == null) {
            throw new RuntimeException(
                    String.format("Error loading function %s: cannot find the %s() method.",
                            m_functionName, methodName));
        }
        else {
            return temp_method;
        }
    }

    public void start() throws Throwable {
        addFunctionInstance();
        m_startMethod.invoke(m_functionInstances.lastElement());
    }

    public void assemble(ByteBuffer udfBuffer, int udafIndex) throws Throwable {
        Object[] paramsIn = new Object[m_paramCount];
        for (int i = 0; i < m_paramCount; i++) {
            paramsIn[i] = getValueFromBuffer(udfBuffer, m_paramTypes[i]);
            if (m_boxUpByteArray[i]) {
                paramsIn[i] = SerializationHelper.boxUpByteArray((byte[])paramsIn[i]);
            }
        }
        m_assembleMethod.invoke(m_functionInstances.get(udafIndex), paramsIn);
    }

    public void combine(Object other, int udafIndex) throws Throwable {
        m_combineMethod.invoke(m_functionInstances.get(udafIndex), other);
    }

    public Object end(int udafIndex) throws Throwable {
        Object result = m_endMethod.invoke(m_functionInstances.get(udafIndex));
        if (udafIndex == m_functionInstances.size() - 1) {
            m_functionInstances.clear();
        }
        return result;
    }

    public VoltType getReturnType() {
        return m_returnType;
    }

    public Object getFunctionInstance(int udafIndex) {
        return m_functionInstances.get(udafIndex);
    }

    public void clearFunctionInstance(int udafIndex) {
        if (udafIndex == m_functionInstances.size() - 1) {
            m_functionInstances.clear();
        }
    }
}